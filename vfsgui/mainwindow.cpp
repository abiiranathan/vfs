#include "mainwindow.hpp"

#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <cstddef>

/* =========================================================================
 * VFS Directory Scan Callback
 * ======================================================================= */
static bool listCallback(const char* path, const vfs_stat_t* st, void* userdata) {
    auto* table = static_cast<QTableWidget*>(userdata);
    int row = table->rowCount();
    table->insertRow(row);

    // Column 0: Path
    table->setItem(row, 0, new QTableWidgetItem(QString::fromUtf8(path)));

    // Column 1: Raw Size (hidden, used for sorting)
    auto* rawSizeItem = new QTableWidgetItem();
    rawSizeItem->setData(Qt::DisplayRole, qulonglong(st->size));
    table->setItem(row, 1, rawSizeItem);

    // Column 2: User-Friendly Size
    QString sizeStr;
    if (st->size < 1024) {
        sizeStr = QString("%1 B").arg(st->size);
    } else if (st->size < static_cast<uint64_t>(1024 * 1024)) {
        sizeStr = QString("%1 KB").arg(double(st->size) / 1024.0, 0, 'f', 1);
    } else {
        sizeStr = QString("%1 MB").arg(double(st->size) / (1024.0 * 1024.0), 0, 'f', 1);
    }
    auto* sizeItem = new QTableWidgetItem(sizeStr);
    sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    table->setItem(row, 2, sizeItem);

    // Column 3: Date
    QDateTime dt = QDateTime::fromSecsSinceEpoch(st->modified_at);
    table->setItem(row, 3, new QTableWidgetItem(dt.toString("yyyy-MM-dd hh:mm:ss")));

    return true;
}

/* =========================================================================
 * UI & Class Constructor/Destructor
 * ======================================================================= */
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), m_vfs(nullptr) {
    setupUi();
    updateButtonsState();
}

MainWindow::~MainWindow() {
    closeVfs();
}

void MainWindow::setupUi() {
    setWindowTitle("VFS Native Explorer");
    resize(1000, 600);

    auto* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    auto* mainLayout = new QVBoxLayout(centralWidget);

    // Top Header Row
    auto* topLayout = new QHBoxLayout();
    m_imageLabel = new QLabel("Active: [No VFS Mounted]", this);
    m_imageLabel->setStyleSheet("font-weight: bold; color: #555;");

    m_btnOpen = new QPushButton("Open Container", this);
    m_btnCreate = new QPushButton("New Container", this);

    topLayout->addWidget(m_imageLabel, 1);
    topLayout->addWidget(m_btnOpen);
    topLayout->addWidget(m_btnCreate);
    mainLayout->addLayout(topLayout);

    // Center layout: Table on left, Actions sidebar on right
    auto* centerLayout = new QHBoxLayout();

    // Create a vertical column for the search bar + table
    auto* leftLayout = new QVBoxLayout();

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search files by name...");
    m_searchEdit->setClearButtonEnabled(true);  // Adds a native "X" clear button
    leftLayout->addWidget(m_searchEdit);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({"File Path", "", "Size", "Modified Date"});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->hideSection(1);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setMinimumSectionSize(80);

    leftLayout->addWidget(m_table);
    centerLayout->addLayout(leftLayout, 4);

    auto* sidebarLayout = new QVBoxLayout();
    m_btnAdd = new QPushButton("Add File...", this);
    m_btnExtract = new QPushButton("Extract File...", this);
    m_btnRename = new QPushButton("Rename...", this);
    m_btnDelete = new QPushButton("Delete", this);
    m_btnRefresh = new QPushButton("Refresh", this);

    sidebarLayout->addWidget(m_btnAdd);
    sidebarLayout->addWidget(m_btnExtract);
    sidebarLayout->addWidget(m_btnRename);
    sidebarLayout->addWidget(m_btnDelete);
    sidebarLayout->addWidget(m_btnRefresh);
    sidebarLayout->addStretch();
    centerLayout->addLayout(sidebarLayout, 1);

    mainLayout->addLayout(centerLayout);

    // Bottom Status Bar
    m_statusLabel = new QLabel("Ready. Open or create an archive to begin.", this);
    statusBar()->addWidget(m_statusLabel);

    // Connections
    connect(m_btnOpen, &QPushButton::clicked, this, &MainWindow::onOpenVfs);
    connect(m_btnCreate, &QPushButton::clicked, this, &MainWindow::onCreateVfs);
    connect(m_btnAdd, &QPushButton::clicked, this, &MainWindow::onAddFile);
    connect(m_btnExtract, &QPushButton::clicked, this, &MainWindow::onExtractFile);
    connect(m_btnRename, &QPushButton::clicked, this, &MainWindow::onRenameFile);
    connect(m_btnDelete, &QPushButton::clicked, this, &MainWindow::onDeleteFile);
    connect(m_btnRefresh, &QPushButton::clicked, this, &MainWindow::onRefresh);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &MainWindow::onSelectionChanged);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);
}

/* =========================================================================
 * VFS Functions Mapping
 * ======================================================================= */

void MainWindow::closeVfs() {
    if (m_vfs) {
        vfs_close(m_vfs);
        m_vfs = nullptr;
        m_currentImage.clear();
        m_imageLabel->setText("Active: [No VFS Mounted]");
    }
}

void MainWindow::updateButtonsState() {
    bool hasVfs = (m_vfs != nullptr);
    bool hasSelection = !m_table->selectedItems().isEmpty();

    m_btnAdd->setEnabled(hasVfs);
    m_btnRefresh->setEnabled(hasVfs);

    m_btnExtract->setEnabled(hasVfs && hasSelection);
    m_btnRename->setEnabled(hasVfs && hasSelection);
    m_btnDelete->setEnabled(hasVfs && hasSelection);
}

void MainWindow::onSelectionChanged() {
    updateButtonsState();
}

void MainWindow::refreshList() {
    if (!m_vfs) return;

    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    vfs_list(m_vfs, nullptr, listCallback, m_table);

    m_table->setSortingEnabled(true);
    onSearchChanged(m_searchEdit->text());
    updateButtonsState();
}

void MainWindow::onCreateVfs() {
    QString path = QFileDialog::getSaveFileName(this, "Create VFS Image", "", "VFS Containers (*.vsf *.vfs)");
    if (path.isEmpty()) return;

    closeVfs();

    vfs_status_t s = vfs_create(path.toUtf8().constData(), &m_vfs);
    if (s != VFS_OK) {
        QMessageBox::critical(this, "VFS Error", QString("Failed to create container:\n%1").arg(vfs_strerror(s)));
        m_vfs = nullptr;
    } else {
        m_currentImage = path;
        m_imageLabel->setText(QString("Active: %1").arg(QFileInfo(path).fileName()));
        m_statusLabel->setText("New VFS container initialized.");
        refreshList();
    }
    updateButtonsState();
}

void MainWindow::onOpenVfs() {
    QString path = QFileDialog::getOpenFileName(this, "Open VFS Image", "",
                                                "VFS Containers (*.vsf *.vfs);;All Files (*)");
    if (path.isEmpty()) return;

    closeVfs();

    vfs_status_t s = vfs_open(path.toUtf8().constData(), false, &m_vfs);
    if (s != VFS_OK) {
        QMessageBox::critical(this, "VFS Error", QString("Failed to open container:\n%1").arg(vfs_strerror(s)));
        m_vfs = nullptr;
    } else {
        m_currentImage = path;
        m_imageLabel->setText(QString("Active: %1").arg(QFileInfo(path).fileName()));
        m_statusLabel->setText("VFS container loaded successfully.");
        refreshList();
    }
    updateButtonsState();
}

void MainWindow::setVFSFile(const QString& path) {
    vfs_status_t s = vfs_open(path.toUtf8().constData(), false, &m_vfs);
    if (s != VFS_OK) {
        QMessageBox::critical(this, "VFS Error", QString("Failed to open container:\n%1").arg(vfs_strerror(s)));
        m_vfs = nullptr;
    } else {
        m_currentImage = path;
        m_imageLabel->setText(QString("Active: %1").arg(QFileInfo(path).fileName()));
        m_statusLabel->setText("VFS container loaded successfully.");
        refreshList();
    }
    updateButtonsState();
}

void MainWindow::onAddFile() {
    if (!m_vfs) return;

    QString localPath = QFileDialog::getOpenFileName(this, "Select File to Copy into VFS");
    if (localPath.isEmpty()) return;

    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, "File IO Error", "Failed to open local host file for reading.");
        return;
    }

    QFileInfo info(localPath);
    QString vfsPath = QInputDialog::getText(this, "VFS Destination", "Enter file path inside VFS:", QLineEdit::Normal,
                                            info.fileName());
    if (vfsPath.isEmpty()) return;

    vfs_fd_t vfd = vfs_fopen(m_vfs, vfsPath.toUtf8().constData(), VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (vfd < 0) {
        QMessageBox::critical(this, "VFS API Error",
                              QString("Failed to create VFS file:\n%1").arg(vfs_strerror((vfs_status_t)vfd)));
        return;
    }

    QByteArray fileData = file.readAll();
    size_t written = 0;
    vfs_status_t s = VFS_OK;

    if (fileData.size() > 0) {
        s = vfs_fwrite(m_vfs, vfd, fileData.constData(), static_cast<size_t>(fileData.size()), &written);
    }

    vfs_fclose(m_vfs, vfd);

    if (s != VFS_OK) {
        QMessageBox::critical(this, "VFS API Error", QString("Failed to write data block:\n%1").arg(vfs_strerror(s)));
    } else {
        m_statusLabel->setText(QString("Successfully imported '%1' (%2 bytes)").arg(vfsPath).arg(written));
        refreshList();
    }
}

void MainWindow::onExtractFile() {
    if (!m_vfs) return;

    int row = m_table->currentRow();
    if (row < 0) return;

    QString vfsPath = m_table->item(row, 0)->text();
    QString localPath = QFileDialog::getSaveFileName(this, "Extract File", QFileInfo(vfsPath).fileName());
    if (localPath.isEmpty()) return;

    vfs_fd_t vfd = vfs_fopen(m_vfs, vfsPath.toUtf8().constData(), VFS_O_RDONLY);
    if (vfd < 0) {
        QMessageBox::critical(this, "VFS API Error",
                              QString("Failed to open VFS file:\n%1").arg(vfs_strerror((vfs_status_t)vfd)));
        return;
    }

    QFile localFile(localPath);
    if (!localFile.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this, "File IO Error", "Failed to open host path for writing.");
        vfs_fclose(m_vfs, vfd);
        return;
    }

    char buf[65536];
    size_t bytes_read = 0;
    vfs_status_t s;
    bool writeError = false;

    while ((s = vfs_fread(m_vfs, vfd, buf, sizeof(buf), &bytes_read)) == VFS_OK && bytes_read > 0) {
        if (localFile.write(buf, (qint64)bytes_read) != static_cast<qint64>(bytes_read)) {
            writeError = true;
            break;
        }
    }

    vfs_fclose(m_vfs, vfd);
    localFile.close();

    if (s != VFS_OK) {
        QMessageBox::critical(this, "VFS API Error", QString("Failed reading VFS blocks:\n%1").arg(vfs_strerror(s)));
        localFile.remove();
    } else if (writeError) {
        QMessageBox::critical(this, "File IO Error", "Host disk write error occurred during extraction.");
        localFile.remove();
    } else {
        m_statusLabel->setText(QString("Successfully extracted '%1'").arg(vfsPath));
    }
}

void MainWindow::onDeleteFile() {
    if (!m_vfs) return;

    int row = m_table->currentRow();
    if (row < 0) return;

    QString vfsPath = m_table->item(row, 0)->text();
    auto confirm = QMessageBox::question(this, "Confirm Deletion",
                                         QString("Are you sure you want to delete '%1'?").arg(vfsPath));
    if (confirm != QMessageBox::Yes) return;

    vfs_status_t s = vfs_unlink(m_vfs, vfsPath.toUtf8().constData());
    if (s != VFS_OK) {
        QMessageBox::critical(this, "VFS API Error", QString("Failed to delete resource:\n%1").arg(vfs_strerror(s)));
    } else {
        m_statusLabel->setText(QString("Deleted '%1'").arg(vfsPath));
        refreshList();
    }
}

void MainWindow::onRenameFile() {
    if (!m_vfs) return;

    int row = m_table->currentRow();
    if (row < 0) return;

    QString oldPath = m_table->item(row, 0)->text();
    QString newPath = QInputDialog::getText(this, "Rename Resource", "Enter new path:", QLineEdit::Normal, oldPath);
    if (newPath.isEmpty() || newPath == oldPath) return;

    vfs_status_t s = vfs_rename(m_vfs, oldPath.toUtf8().constData(), newPath.toUtf8().constData());
    if (s != VFS_OK) {
        QMessageBox::critical(this, "VFS API Error", QString("Failed to rename resource:\n%1").arg(vfs_strerror(s)));
    } else {
        m_statusLabel->setText(QString("Renamed '%1' to '%2'").arg(oldPath).arg(newPath));
        refreshList();
    }
}

void MainWindow::onRefresh() {
    refreshList();
    m_statusLabel->setText("View updated.");
}

void MainWindow::onSearchChanged(const QString& text) {
    // Disable sorting temporarily to prevent row reordering during filtering
    m_table->setSortingEnabled(false);

    for (int r = 0; r < m_table->rowCount(); ++r) {
        QTableWidgetItem* pathItem = m_table->item(r, 0);
        if (pathItem) {
            bool matches = pathItem->text().contains(text, Qt::CaseInsensitive);
            m_table->setRowHidden(r, !matches);

            // If a selected row becomes hidden, deselect it to keep states consistent
            if (!matches && pathItem->isSelected()) { m_table->clearSelection(); }
        }
    }

    m_table->setSortingEnabled(true);
}