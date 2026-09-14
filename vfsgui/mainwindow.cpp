#include "mainwindow.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFontDatabase>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStyle>
#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

/* =========================================================================
 * Fast bulk transfer + parallel dispatch
 *
 * Mirrors the `vfs-cli pack` pipeline: whole files move through
 * vfs_import_fd()/vfs_export_fd() (one kernel-side copy per extent,
 * reflinked metadata-only where the filesystem supports it) instead of
 * userspace 64 KiB read/write loops, and independent files are processed
 * by a fixed worker pool. The library's per-inode locks make concurrent
 * transfers on different files safe.
 * ======================================================================= */

/** Worker count: online CPUs, clamped to jobs and to 64. */
static unsigned workerCount(size_t jobCount) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) {
        hw = 1;
    }
    if (hw > 64) {
        hw = 64;
    }
    if (static_cast<size_t>(hw) > jobCount) {
        hw = static_cast<unsigned>(jobCount);
    }
    return hw;
}

/**
 * Runs fn(i) for i in [0, jobCount) across a fixed pool driven by one
 * atomic cursor, stopping early if @p canceled is set. The job list is
 * fully known before dispatch, so a flat cursor has no contention and no
 * idle worker ever spins.
 */
template <typename Fn>
static void parallelFor(size_t jobCount, const std::atomic<bool>& canceled, Fn&& fn) {
    if (jobCount == 0) {
        return;
    }
    unsigned n = workerCount(jobCount);
    if (n <= 1) {
        for (size_t i = 0; i < jobCount && !canceled.load(); ++i) {
            fn(i);
        }
        return;
    }

    std::atomic<size_t> cursor{0};
    std::vector<std::thread> pool;
    pool.reserve(n);
    for (unsigned t = 0; t < n; ++t) {
        pool.emplace_back([&]() {
            for (;;) {
                if (canceled.load()) {
                    return;
                }
                size_t i = cursor.fetch_add(1, std::memory_order_relaxed);
                if (i >= jobCount) {
                    return;
                }
                fn(i);
            }
        });
    }
    for (auto& th : pool) {
        th.join();
    }
}

/**
 * Imports one host file into the VFS through the kernel-side bulk path.
 * Uses fstat() on the opened descriptor to avoid symlink/size races; the
 * source is never mmap'd, so a concurrently shrinking file fails cleanly.
 */
static bool importHostFileRaw(vfs_t* vfs, const char* hostPath, const char* vfsPath) {
    int hfd = ::open(hostPath, O_RDONLY | O_CLOEXEC);
    if (hfd < 0) {
        return false;
    }

    struct stat st;
    if (::fstat(hfd, &st) < 0 || !S_ISREG(st.st_mode)) {
        ::close(hfd);
        return false;
    }

    vfs_fd_t vfd = vfs_fopen(vfs, vfsPath, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (vfd < 0) {
        ::close(hfd);
        return false;
    }

    vfs_status_t s = VFS_OK;
    if (st.st_size > 0) {
        s = vfs_import_fd(vfs, vfd, hfd, static_cast<uint64_t>(st.st_size));
    }

    ::close(hfd);
    vfs_fclose(vfs, vfd);
    return s == VFS_OK;
}

/**
 * Exports one VFS file to host disk through the kernel-side bulk path;
 * sparse holes materialise as zeros for free, and a partial destination
 * is unlinked on failure so no truncated file is left behind.
 */
static bool exportVfsFileRaw(vfs_t* vfs, const char* vfsPath, const char* hostPath) {
    vfs_stat_t st;
    if (vfs_stat(vfs, vfsPath, &st) != VFS_OK) {
        return false;
    }

    int hfd = ::open(hostPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (hfd < 0) {
        return false;
    }

    vfs_fd_t vfd = vfs_fopen(vfs, vfsPath, VFS_O_RDONLY);
    if (vfd < 0) {
        ::close(hfd);
        return false;
    }

    size_t sent = 0;
    off_t off = 0;
    vfs_status_t s = VFS_OK;
    if (st.size > 0) {
        s = vfs_export_fd(vfs, hfd, vfd, &off, static_cast<size_t>(st.size), &sent);
    }

    ::close(hfd);
    vfs_fclose(vfs, vfd);

    if (s != VFS_OK || sent != static_cast<size_t>(st.size)) {
        ::unlink(hostPath);
        return false;
    }
    return true;
}

/** QString convenience wrappers for the single-file UI operations. */
static bool writeHostFileToVfsHandle(vfs_t* vfs, const QString& hostPath, const QString& vfsPath) {
    return importHostFileRaw(vfs, hostPath.toUtf8().constData(), vfsPath.toUtf8().constData());
}

static bool extractVfsFileToHostPath(vfs_t* vfs, const QString& vfsPath, const QString& hostPath) {
    return exportVfsFileRaw(vfs, vfsPath.toUtf8().constData(), hostPath.toUtf8().constData());
}

/* =========================================================================
 * PackWorker Implementation (Async)
 * ======================================================================= */
PackWorker::PackWorker(QString dirPath, QString containerPath, QObject* parent)
    : QObject(parent), m_dirPath(std::move(dirPath)), m_containerPath(std::move(containerPath)) {}

void PackWorker::process() {
    // 1. Enumerate files to get total count
    QStringList filesToPack;
    QDir rootDir(m_dirPath);
    QDirIterator countIt(m_dirPath, QDir::Files, QDirIterator::Subdirectories);
    while (countIt.hasNext()) {
        if (m_canceled.load()) {
            emit finished(false, 0, 0, "Operation canceled by user.");
            return;
        }
        filesToPack.append(countIt.next());
    }

    int totalFiles = static_cast<int>(filesToPack.size());
    if (totalFiles == 0) {
        emit finished(false, 0, 0, "Selected folder is empty.");
        return;
    }

    // 2. Resolve every destination path up front so the parallel workers
    //    share only the immutable job list.
    struct PackJob {
        QByteArray hostPath;
        QByteArray vfsPath;
        QString label;
    };
    std::vector<PackJob> jobs;
    jobs.reserve(static_cast<size_t>(filesToPack.size()));
    for (int i = 0; i < totalFiles; ++i) {
        const QString& hostFilePath = filesToPack.at(i);
        QString relPath = rootDir.relativeFilePath(hostFilePath);
        jobs.push_back({hostFilePath.toUtf8(), ("/" + relPath).toUtf8(), relPath});
    }

    // 3. Initialize fresh container
    vfs_t* vfs = nullptr;
    vfs_status_t s = vfs_create(m_containerPath.toUtf8().constData(), &vfs);
    if (s != VFS_OK) {
        emit finished(false, 0, 0, QString("vfs_create failed: %1").arg(vfs_strerror(s)));
        return;
    }

    // 4. Import files in parallel, one kernel-side bulk copy per extent.
    std::atomic<size_t> packedCount{0};
    std::atomic<size_t> errorCount{0};
    std::atomic<size_t> completed{0};

    parallelFor(jobs.size(), m_canceled, [&](size_t i) {
        const PackJob& job = jobs[i];
        if (importHostFileRaw(vfs, job.hostPath.constData(), job.vfsPath.constData())) {
            packedCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            errorCount.fetch_add(1, std::memory_order_relaxed);
        }
        size_t done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
        emit progress(static_cast<int>(done), totalFiles, job.label);
    });

    vfs_sync(vfs);
    vfs_close(vfs);

    if (m_canceled.load()) {
        QFile::remove(m_containerPath);
        emit finished(false, packedCount.load(), errorCount.load(), "Operation canceled by user.");
        return;
    }

    emit finished(true, packedCount.load(), errorCount.load(), "");
}

/* =========================================================================
 * UnpackWorker Implementation (Async)
 * ======================================================================= */
UnpackWorker::UnpackWorker(QString containerPath, QString outDir, QObject* parent)
    : QObject(parent), m_containerPath(std::move(containerPath)), m_outDir(std::move(outDir)) {}

struct UnpackScanData {
    QStringList paths;
};

static auto scanCallback(const char* path, const vfs_stat_t* st, void* userdata) -> bool {
    Q_UNUSED(st);
    auto* d = static_cast<UnpackScanData*>(userdata);
    d->paths.append(QString::fromUtf8(path));
    return true;
}

void UnpackWorker::process() {
    vfs_t* vfs = nullptr;
    vfs_status_t s = vfs_open(m_containerPath.toUtf8().constData(), true, &vfs);
    if (s != VFS_OK) {
        emit finished(false, 0, 0, QString("vfs_open failed: %1").arg(vfs_strerror(s)));
        return;
    }

    UnpackScanData scanData;
    vfs_list(vfs, nullptr, scanCallback, &scanData);

    int totalFiles = static_cast<int>(scanData.paths.size());

    // Resolve destinations and create parent directories up front (so the
    // parallel workers only perform bulk transfers).
    struct UnpackJob {
        QByteArray vfsPath;
        QByteArray hostPath;
        QString label;
    };
    std::vector<UnpackJob> jobs;
    jobs.reserve(static_cast<size_t>(scanData.paths.size()));
    for (int i = 0; i < totalFiles; ++i) {
        const QString& vfsPath = scanData.paths.at(i);
        QString relPath = vfsPath.startsWith('/') ? vfsPath.mid(1) : vfsPath;
        QString hostPath = QDir(m_outDir).filePath(relPath);
        QFileInfo fi(hostPath);
        QDir().mkpath(fi.absolutePath());
        jobs.push_back({vfsPath.toUtf8(), hostPath.toUtf8(), relPath});
    }

    std::atomic<size_t> unpackedCount{0};
    std::atomic<size_t> errorCount{0};
    std::atomic<size_t> completed{0};

    parallelFor(jobs.size(), m_canceled, [&](size_t i) {
        const UnpackJob& job = jobs[i];
        if (exportVfsFileRaw(vfs, job.vfsPath.constData(), job.hostPath.constData())) {
            unpackedCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            errorCount.fetch_add(1, std::memory_order_relaxed);
        }
        size_t done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
        emit progress(static_cast<int>(done), totalFiles, job.label);
    });

    vfs_close(vfs);

    if (m_canceled.load()) {
        emit finished(false, unpackedCount.load(), errorCount.load(), "Operation canceled by user.");
        return;
    }

    emit finished(true, unpackedCount.load(), errorCount.load(), "");
}

/* =========================================================================
 * VFS Directory Scan Callback (Main Table)
 * ======================================================================= */
static auto listCallback(const char* path, const vfs_stat_t* st, void* userdata) -> bool {
    auto* table = static_cast<QTableWidget*>(userdata);
    int row = table->rowCount();
    table->insertRow(row);

    // Path
    auto* pathItem = new QTableWidgetItem(QString::fromUtf8(path));
    pathItem->setIcon(qApp->style()->standardIcon(QStyle::SP_FileIcon));
    table->setItem(row, 0, pathItem);

    // Hidden raw size for numeric sorting
    auto* rawSizeItem = new QTableWidgetItem();
    rawSizeItem->setData(Qt::DisplayRole, static_cast<qulonglong>(st->size));
    table->setItem(row, 1, rawSizeItem);

    // Human-readable size
    QString sizeStr;
    if (st->size < 1024) {
        sizeStr = QString("%1 B").arg(st->size);
    } else if (st->size < (1024ULL * 1024ULL)) {
        sizeStr = QString("%1 KB").arg(static_cast<double>(st->size) / 1024.0, 0, 'f', 1);
    } else if (st->size < (1024ULL * 1024ULL * 1024ULL)) {
        sizeStr = QString("%1 MB").arg(static_cast<double>(st->size) / (1024.0 * 1024.0), 0, 'f', 2);
    } else {
        sizeStr = QString("%1 GB").arg(static_cast<double>(st->size) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    }
    auto* sizeItem = new QTableWidgetItem(sizeStr);
    sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    table->setItem(row, 2, sizeItem);

    // Block Count
    auto* blockItem = new QTableWidgetItem(QString::number(st->block_count));
    blockItem->setTextAlignment(Qt::AlignCenter);
    table->setItem(row, 3, blockItem);

    // Date Modified
    QDateTime dt = QDateTime::fromSecsSinceEpoch(st->modified_at);
    auto* dateItem = new QTableWidgetItem(dt.toString("yyyy-MM-dd hh:mm:ss"));
    table->setItem(row, 4, dateItem);

    return true;
}

/* =========================================================================
 * Main Window Constructor / Setup
 * ======================================================================= */
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setupUi();
    setupMenusAndToolbars();
    updateButtonsState();
}

MainWindow::~MainWindow() { closeVfs(); }

void MainWindow::setupUi() {
    setWindowTitle("VFS Native Explorer v3");
    resize(1200, 720);

    auto* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    auto* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(6, 6, 6, 6);

    auto* splitter = new QSplitter(Qt::Horizontal, this);

    // ---- Left Pane: Search + Table ----
    auto* leftContainer = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftContainer);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Filter files by name or path (/)...");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->addAction(style()->standardIcon(QStyle::SP_FileDialogContentsView), QLineEdit::LeadingPosition);
    leftLayout->addWidget(m_searchEdit);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({"Path", "", "Size", "Blocks", "Modified Date"});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->hideSection(1);  // Hidden sort column
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);

    leftLayout->addWidget(m_table);
    splitter->addWidget(leftContainer);

    // ---- Right Pane: Tabbed Preview & Inspector ----
    m_previewTabs = new QTabWidget(this);

    // 1. Text & Code Preview
    m_textPreview = new QPlainTextEdit(this);
    m_textPreview->setReadOnly(true);
    m_textPreview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_previewTabs->addTab(m_textPreview, style()->standardIcon(QStyle::SP_FileIcon), "Text Preview");

    // 2. Image Preview
    m_imageScrollArea = new QScrollArea(this);
    m_imageScrollArea->setAlignment(Qt::AlignCenter);
    m_imagePreviewLabel = new QLabel(this);
    m_imagePreviewLabel->setAlignment(Qt::AlignCenter);
    m_imageScrollArea->setWidget(m_imagePreviewLabel);
    m_imageScrollArea->setWidgetResizable(true);
    m_previewTabs->addTab(m_imageScrollArea, style()->standardIcon(QStyle::SP_DesktopIcon), "Image Preview");

    // 3. Hex Viewer
    m_hexPreview = new QPlainTextEdit(this);
    m_hexPreview->setReadOnly(true);
    m_hexPreview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_previewTabs->addTab(m_hexPreview, style()->standardIcon(QStyle::SP_FileDialogDetailedView), "Hex View");

    // 4. Inode & Stat Properties
    m_statInfoView = new QPlainTextEdit(this);
    m_statInfoView->setReadOnly(true);
    m_statInfoView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_previewTabs->addTab(m_statInfoView, style()->standardIcon(QStyle::SP_FileDialogInfoView), "Properties");

    splitter->addWidget(m_previewTabs);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    mainLayout->addWidget(splitter);

    // ---- Status Bar ----
    m_statusLabel = new QLabel("Ready. Open or create a container.", this);
    m_storageInfoLabel = new QLabel("No Container Loaded", this);
    m_storageInfoLabel->setStyleSheet("color: #777; font-weight: bold; padding-right: 10px;");

    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_storageInfoLabel);

    // Signals
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &MainWindow::onSelectionChanged);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, &MainWindow::onTableItemDoubleClicked);
    connect(m_table, &QTableWidget::customContextMenuRequested, this, &MainWindow::onContextMenuRequested);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);
}

void MainWindow::setupMenusAndToolbars() {
    auto* toolBar = addToolBar("Main Operations");
    toolBar->setMovable(false);
    toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    // File Menu
    auto* fileMenu = menuBar()->addMenu("&File");

    m_actNew = fileMenu->addAction(style()->standardIcon(QStyle::SP_FileIcon), "&New Container...", this,
                                   &MainWindow::onCreateVfs);
    m_actNew->setShortcut(QKeySequence::New);

    m_actOpen = fileMenu->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton), "&Open Container...", this,
                                    &MainWindow::onOpenVfs);
    m_actOpen->setShortcut(QKeySequence::Open);

    m_actClose = fileMenu->addAction(style()->standardIcon(QStyle::SP_DialogCloseButton), "&Close Container", this,
                                     &MainWindow::onCloseVfs);
    fileMenu->addSeparator();

    m_actPack = fileMenu->addAction(style()->standardIcon(QStyle::SP_FileDialogNewFolder), "&Pack Directory...", this,
                                    &MainWindow::onPackDirectory);
    m_actUnpack = fileMenu->addAction(style()->standardIcon(QStyle::SP_DriveHDIcon), "&Unpack Container...", this,
                                      &MainWindow::onUnpackContainer);
    fileMenu->addSeparator();

    auto* actExit =
        fileMenu->addAction(style()->standardIcon(QStyle::SP_TitleBarCloseButton), "E&xit", this, &QWidget::close);
    actExit->setShortcut(QKeySequence::Quit);

    // Actions Menu
    auto* actMenu = menuBar()->addMenu("&Actions");
    m_actAdd = actMenu->addAction(style()->standardIcon(QStyle::SP_FileDialogToParent), "&Import File...", this,
                                  &MainWindow::onAddFile);
    m_actExtract = actMenu->addAction(style()->standardIcon(QStyle::SP_DialogSaveButton), "&Export File...", this,
                                      &MainWindow::onExtractFile);
    m_actRename = actMenu->addAction(style()->standardIcon(QStyle::SP_FileDialogContentsView), "&Rename / Move...",
                                     this, &MainWindow::onRenameFile);
    m_actDelete =
        actMenu->addAction(style()->standardIcon(QStyle::SP_TrashIcon), "&Delete", this, &MainWindow::onDeleteFile);
    m_actDelete->setShortcut(QKeySequence::Delete);

    actMenu->addSeparator();
    m_actRefresh =
        actMenu->addAction(style()->standardIcon(QStyle::SP_BrowserReload), "&Refresh", this, &MainWindow::onRefresh);
    m_actRefresh->setShortcut(QKeySequence::Refresh);

    // Tools Menu
    auto* toolsMenu = menuBar()->addMenu("&Tools");
    m_actDump = toolsMenu->addAction(style()->standardIcon(QStyle::SP_FileDialogInfoView),
                                     "&Superblock & Inode Diagnostics", this, &MainWindow::onDumpSuperblock);

    // Toolbar
    toolBar->addAction(m_actNew);
    toolBar->addAction(m_actOpen);
    toolBar->addSeparator();
    toolBar->addAction(m_actPack);
    toolBar->addAction(m_actUnpack);
    toolBar->addSeparator();
    toolBar->addAction(m_actAdd);
    toolBar->addAction(m_actExtract);
    toolBar->addAction(m_actDelete);
    toolBar->addAction(m_actRefresh);
    toolBar->addSeparator();
    toolBar->addAction(m_actDump);
}

/* =========================================================================
 * VFS Lifecycle
 * ======================================================================= */
void MainWindow::closeVfs() {
    if (m_vfs != nullptr) {
        vfs_close(m_vfs);
        m_vfs = nullptr;
        m_currentImage.clear();
        m_table->setRowCount(0);
        clearPreviewPane();
        m_storageInfoLabel->setText("No Container Loaded");
        m_statusLabel->setText("Container closed.");
        setWindowTitle("VFS Native Explorer v3");
    }
}

void MainWindow::onCloseVfs() {
    closeVfs();
    updateButtonsState();
}

void MainWindow::updateButtonsState() {
    bool hasVfs = (m_vfs != nullptr) && !m_isOperationRunning;
    bool hasSelection = !m_table->selectedItems().isEmpty() && !m_isOperationRunning;

    m_actNew->setEnabled(!m_isOperationRunning);
    m_actOpen->setEnabled(!m_isOperationRunning);
    m_actPack->setEnabled(!m_isOperationRunning);
    m_actClose->setEnabled(hasVfs);
    m_actUnpack->setEnabled(hasVfs);
    m_actAdd->setEnabled(hasVfs);
    m_actRefresh->setEnabled(hasVfs);
    m_actDump->setEnabled(hasVfs);

    m_actExtract->setEnabled(hasVfs && hasSelection);
    m_actRename->setEnabled(hasVfs && hasSelection);
    m_actDelete->setEnabled(hasVfs && hasSelection);
}

void MainWindow::refreshList() {
    if (m_vfs == nullptr) {
        return;
    }

    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    vfs_list(m_vfs, nullptr, listCallback, m_table);

    m_table->setSortingEnabled(true);
    onSearchChanged(m_searchEdit->text());
    updateButtonsState();

    m_storageInfoLabel->setText(
        QString("Container: %1 (%2 files)").arg(QFileInfo(m_currentImage).fileName()).arg(m_table->rowCount()));
}

void MainWindow::setVFSFile(const QString& path) {
    closeVfs();
    vfs_status_t s = vfs_open(path.toUtf8().constData(), false, &m_vfs);
    if (s != VFS_OK) {
        QMessageBox::critical(this, "VFS Error", QString("Failed to open VFS container:\n%1").arg(vfs_strerror(s)));
        m_vfs = nullptr;
    } else {
        m_currentImage = path;
        setWindowTitle(QString("VFS Explorer — %1").arg(QFileInfo(path).fileName()));
        m_statusLabel->setText("VFS container loaded successfully.");
        refreshList();
    }
    updateButtonsState();
}

void MainWindow::onCreateVfs() {
    QString path = QFileDialog::getSaveFileName(this, "Create New VFS Container", "", "VFS Container (*.vfs *.vsf)");
    if (path.isEmpty()) {
        return;
    }

    closeVfs();
    vfs_status_t s = vfs_create(path.toUtf8().constData(), &m_vfs);
    if (s != VFS_OK) {
        QMessageBox::critical(this, "VFS Error", QString("Failed to create container:\n%1").arg(vfs_strerror(s)));
        m_vfs = nullptr;
    } else {
        m_currentImage = path;
        setWindowTitle(QString("VFS Explorer — %1").arg(QFileInfo(path).fileName()));
        m_statusLabel->setText("New container initialized.");
        refreshList();
    }
    updateButtonsState();
}

void MainWindow::onOpenVfs() {
    QString path =
        QFileDialog::getOpenFileName(this, "Open VFS Container", "", "VFS Container (*.vfs *.vsf);;All Files (*)");
    if (path.isEmpty()) {
        return;
    }
    setVFSFile(path);
}

/* =========================================================================
 * Pack & Unpack (Fully Non-Blocking / Async with QThread + QProgressDialog)
 * ======================================================================= */
void MainWindow::onPackDirectory() {
    QString dirPath = QFileDialog::getExistingDirectory(this, "Select Directory to Pack");
    if (dirPath.isEmpty()) {
        return;
    }

    QString containerPath =
        QFileDialog::getSaveFileName(this, "Select Target VFS Image", "", "VFS Container (*.vfs *.vsf)");
    if (containerPath.isEmpty()) {
        return;
    }

    closeVfs();
    m_isOperationRunning = true;
    updateButtonsState();

    auto* progressDialog = new QProgressDialog("Scanning files to pack...", "Cancel", 0, 100, this);
    progressDialog->setWindowTitle("Packing Directory");
    progressDialog->setWindowModality(Qt::WindowModal);
    progressDialog->setMinimumDuration(0);
    progressDialog->setValue(0);

    auto* thread = new QThread(this);
    auto* worker = new PackWorker(dirPath, containerPath);
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &PackWorker::process);
    connect(worker, &PackWorker::progress, this, [progressDialog](int current, int total, const QString& currentFile) {
        progressDialog->setMaximum(total);
        progressDialog->setValue(current);
        progressDialog->setLabelText(QString("Packing (%1/%2):\n%3").arg(current).arg(total).arg(currentFile));
    });

    connect(progressDialog, &QProgressDialog::canceled, worker, &PackWorker::cancel, Qt::DirectConnection);

    connect(
        worker, &PackWorker::finished, this,
        [this, thread, worker, progressDialog, containerPath](bool success, size_t packedCount, size_t errorCount,
                                                              const QString& errorMsg) {
            thread->quit();
            thread->wait();
            worker->deleteLater();
            thread->deleteLater();
            progressDialog->deleteLater();

            m_isOperationRunning = false;
            updateButtonsState();

            if (!success) {
                QMessageBox::warning(this, "Pack Aborted",
                                     QString("Pack operation was not completed: %1").arg(errorMsg));
                return;
            }

            if (errorCount > 0) {
                QMessageBox::warning(this, "Pack Finished With Errors",
                                     QString("Packed %1 file(s) with %2 error(s).").arg(packedCount).arg(errorCount));
            } else {
                QMessageBox::information(this, "Pack Successful",
                                         QString("Successfully packed %1 file(s) into container.").arg(packedCount));
            }

            setVFSFile(containerPath);
        });

    thread->start();
}

void MainWindow::onUnpackContainer() {
    if (!m_vfs || m_currentImage.isEmpty()) {
        return;
    }

    QString outDir = QFileDialog::getExistingDirectory(this, "Select Destination Directory to Unpack");
    if (outDir.isEmpty()) {
        return;
    }

    m_isOperationRunning = true;
    updateButtonsState();

    auto* progressDialog = new QProgressDialog("Preparing unpack...", "Cancel", 0, 100, this);
    progressDialog->setWindowTitle("Unpacking Container");
    progressDialog->setWindowModality(Qt::WindowModal);
    progressDialog->setMinimumDuration(0);
    progressDialog->setValue(0);

    auto* thread = new QThread(this);
    auto* worker = new UnpackWorker(m_currentImage, outDir);
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &UnpackWorker::process);
    connect(
        worker, &UnpackWorker::progress, this, [progressDialog](int current, int total, const QString& currentFile) {
            progressDialog->setMaximum(total);
            progressDialog->setValue(current);
            progressDialog->setLabelText(QString("Extracting (%1/%2):\n%3").arg(current).arg(total).arg(currentFile));
        });

    connect(progressDialog, &QProgressDialog::canceled, worker, &UnpackWorker::cancel, Qt::DirectConnection);

    connect(worker, &UnpackWorker::finished, this,
            [this, thread, worker, progressDialog](bool success, size_t unpackedCount, size_t errorCount,
                                                   const QString& errorMsg) {
                thread->quit();
                thread->wait();
                worker->deleteLater();
                thread->deleteLater();
                progressDialog->deleteLater();

                m_isOperationRunning = false;
                updateButtonsState();

                if (!success) {
                    QMessageBox::warning(this, "Unpack Aborted", QString("Unpack operation stopped: %1").arg(errorMsg));
                    return;
                }

                if (errorCount > 0) {
                    QMessageBox::warning(
                        this, "Unpack Completed With Errors",
                        QString("Extracted %1 file(s) with %2 error(s).").arg(unpackedCount).arg(errorCount));
                } else {
                    QMessageBox::information(this, "Unpack Successful",
                                             QString("Successfully extracted %1 file(s).").arg(unpackedCount));
                }
            });

    thread->start();
}

/* =========================================================================
 * Diagnostics & Dump
 * ======================================================================= */
void MainWindow::onDumpSuperblock() {
    if (!m_vfs) {
        return;
    }

    char buf[16384];
    FILE* stream = fmemopen(buf, sizeof(buf), "w");
    if (!stream) {
        return;
    }

    vfs_dump(m_vfs, stream);
    (void)fclose(stream);

    auto* diagDialog = new QDialog(this);
    diagDialog->setWindowTitle("VFS Superblock & Inode Diagnostic Dump");
    diagDialog->resize(800, 500);

    auto* layout = new QVBoxLayout(diagDialog);
    auto* text = new QPlainTextEdit(diagDialog);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setReadOnly(true);
    text->setPlainText(QString::fromUtf8(buf));
    layout->addWidget(text);

    auto* btnClose = new QPushButton("Close", diagDialog);
    connect(btnClose, &QPushButton::clicked, diagDialog, &QDialog::accept);
    layout->addWidget(btnClose, 0, Qt::AlignRight);

    diagDialog->exec();
}

/* =========================================================================
 * Single File Operations
 * ======================================================================= */
bool MainWindow::importHostFile(const QString& hostPath, const QString& vfsPath) {
    return writeHostFileToVfsHandle(m_vfs, hostPath, vfsPath);
}

bool MainWindow::exportVfsFile(const QString& vfsPath, const QString& hostPath) {
    return extractVfsFileToHostPath(m_vfs, vfsPath, hostPath);
}

void MainWindow::onAddFile() {
    if (!m_vfs) {
        return;
    }

    QString localPath = QFileDialog::getOpenFileName(this, "Select File to Import");
    if (localPath.isEmpty()) {
        return;
    }

    QFileInfo fi(localPath);
    QString vfsPath =
        QInputDialog::getText(this, "VFS Path", "Target Path inside VFS:", QLineEdit::Normal, "/" + fi.fileName());
    if (vfsPath.isEmpty()) {
        return;
    }

    if (!importHostFile(localPath, vfsPath)) {
        QMessageBox::critical(this, "Import Error", "Failed to write host file to VFS image.");
    } else {
        m_statusLabel->setText(QString("Imported %1 successfully.").arg(vfsPath));
        refreshList();
    }
}

void MainWindow::onExtractFile() {
    if (!m_vfs) {
        return;
    }
    int row = m_table->currentRow();
    if (row < 0) {
        return;
    }

    QString vfsPath = m_table->item(row, 0)->text();
    QFileInfo fi(vfsPath);
    QString localPath = QFileDialog::getSaveFileName(this, "Export File to Host", fi.fileName());
    if (localPath.isEmpty()) {
        return;
    }

    if (!exportVfsFile(vfsPath, localPath)) {
        QMessageBox::critical(this, "Export Error", "Failed to extract VFS file.");
    } else {
        m_statusLabel->setText(QString("Exported %1 successfully.").arg(vfsPath));
    }
}

void MainWindow::onDeleteFile() {
    if (!m_vfs) {
        return;
    }
    int row = m_table->currentRow();
    if (row < 0) {
        return;
    }

    QString vfsPath = m_table->item(row, 0)->text();
    if (QMessageBox::question(this, "Confirm Delete", QString("Delete virtual resource '%1'?").arg(vfsPath)) !=
        QMessageBox::Yes) {
        return;
    }

    vfs_status_t s = vfs_unlink(m_vfs, vfsPath.toUtf8().constData());
    if (s != VFS_OK) {
        QMessageBox::critical(this, "Delete Error", QString("Failed to delete:\n%1").arg(vfs_strerror(s)));
    } else {
        m_statusLabel->setText(QString("Deleted %1").arg(vfsPath));
        clearPreviewPane();
        refreshList();
    }
}

void MainWindow::onRenameFile() {
    if (!m_vfs) {
        return;
    }
    int row = m_table->currentRow();
    if (row < 0) {
        return;
    }

    QString oldPath = m_table->item(row, 0)->text();
    QString newPath = QInputDialog::getText(this, "Rename / Move", "New path:", QLineEdit::Normal, oldPath);
    if (newPath.isEmpty() || newPath == oldPath) {
        return;
    }

    vfs_status_t s = vfs_rename(m_vfs, oldPath.toUtf8().constData(), newPath.toUtf8().constData());
    if (s != VFS_OK) {
        QMessageBox::critical(this, "Rename Error", QString("Failed to rename:\n%1").arg(vfs_strerror(s)));
    } else {
        m_statusLabel->setText(QString("Renamed %1 -> %2").arg(oldPath, newPath));
        refreshList();
    }
}

void MainWindow::onRefresh() {
    refreshList();
    m_statusLabel->setText("View refreshed.");
}

/* =========================================================================
 * Live Preview & Formatting
 * ======================================================================= */
void MainWindow::clearPreviewPane() {
    m_textPreview->clear();
    m_imagePreviewLabel->clear();
    m_hexPreview->clear();
    m_statInfoView->clear();
}

QString MainWindow::formatHexDump(const QByteArray& data, int maxBytes) {
    QString out;
    int len = std::min(static_cast<int>(data.size()), maxBytes);

    for (int i = 0; i < len; i += 16) {
        out += QString("%1  ").arg(i, 8, 16, QChar('0'));
        QString ascii;

        for (int j = 0; j < 16; ++j) {
            if (i + j < len) {
                auto b = static_cast<uint8_t>(data[i + j]);
                out += QString("%1 ").arg(b, 2, 16, QChar('0')).toUpper();
                ascii += (b >= 32 && b <= 126) ? QChar(b) : QChar('.');
            } else {
                out += "   ";
            }
            if (j == 7) {
                out += " ";
            }
        }
        out += " |" + ascii + "|\n";
    }

    if (data.size() > maxBytes) {
        out += QString("\n... [Showing %1 of %2 bytes] ...").arg(maxBytes).arg(data.size());
    }
    return out;
}

void MainWindow::updatePreviewPane(const QString& vfsPath) {
    clearPreviewPane();
    if (!m_vfs || vfsPath.isEmpty()) {
        return;
    }

    // 1. Fetch metadata
    vfs_stat_t st;
    if (vfs_stat(m_vfs, vfsPath.toUtf8().constData(), &st) != VFS_OK) {
        return;
    }

    QString props = QString(
                        "Path             : %1\n"
                        "Size (Bytes)     : %2\n"
                        "Allocated Blocks : %3 (%4 KB)\n"
                        "Created Date     : %5\n"
                        "Modified Date    : %6\n")
                        .arg(vfsPath)
                        .arg(st.size)
                        .arg(st.block_count)
                        .arg(st.block_count * 4)
                        .arg(QDateTime::fromSecsSinceEpoch(st.created_at).toString("yyyy-MM-dd hh:mm:ss"))
                        .arg(QDateTime::fromSecsSinceEpoch(st.modified_at).toString("yyyy-MM-dd hh:mm:ss"));
    m_statInfoView->setPlainText(props);

    // 2. Read contents up to 4MB for preview
    size_t readLimit = std::min(static_cast<size_t>(st.size), static_cast<size_t>(4 * 1024 * 1024));
    QByteArray data(static_cast<int>(readLimit), '\0');

    vfs_fd_t fd = vfs_fopen(m_vfs, vfsPath.toUtf8().constData(), VFS_O_RDONLY);
    if (fd < 0) {
        return;
    }

    size_t bytesRead = 0;
    vfs_fread(m_vfs, fd, data.data(), readLimit, &bytesRead);
    vfs_fclose(m_vfs, fd);
    data.resize(static_cast<int>(bytesRead));

    // Hex view is always available
    m_hexPreview->setPlainText(formatHexDump(data));

    // Image preview
    QPixmap pixmap;
    if (pixmap.loadFromData(data)) {
        m_imagePreviewLabel->setPixmap(
            pixmap.scaled(m_imageScrollArea->size() * 0.9, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_previewTabs->setCurrentIndex(1);  // Jump to Image tab
        return;
    }

    // Text preview (detect UTF-8 / ASCII)
    bool isBinary = data.contains('\0');
    if (!isBinary) {
        m_textPreview->setPlainText(QString::fromUtf8(data));
        m_previewTabs->setCurrentIndex(0);  // Jump to Text tab
    } else {
        m_textPreview->setPlainText("[Binary data detected. See Hex View tab.]");
        m_previewTabs->setCurrentIndex(2);  // Jump to Hex tab
    }
}

void MainWindow::onSelectionChanged() {
    updateButtonsState();
    int row = m_table->currentRow();
    if (row >= 0 && row < m_table->rowCount()) {
        updatePreviewPane(m_table->item(row, 0)->text());
    }
}

void MainWindow::onTableItemDoubleClicked(int row, int column) {
    Q_UNUSED(column);
    if (row >= 0 && row < m_table->rowCount()) {
        updatePreviewPane(m_table->item(row, 0)->text());
    }
}

void MainWindow::onSearchChanged(const QString& text) {
    m_table->setSortingEnabled(false);
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QTableWidgetItem* item = m_table->item(r, 0);
        if (item) {
            bool match = item->text().contains(text, Qt::CaseInsensitive);
            m_table->setRowHidden(r, !match);
            if (!match && item->isSelected()) {
                m_table->clearSelection();
            }
        }
    }
    m_table->setSortingEnabled(true);
}

void MainWindow::onContextMenuRequested(const QPoint& pos) {
    if (!m_vfs || m_table->selectedItems().isEmpty()) {
        return;
    }

    QMenu menu(this);
    menu.addAction(m_actExtract);
    menu.addAction(m_actRename);
    menu.addAction(m_actDelete);
    menu.addSeparator();
    menu.addAction(m_actRefresh);
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}
