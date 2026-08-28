#pragma once

#include <QDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QToolBar>
#include <QVBoxLayout>
#include <atomic>

#include "vfs.h"

/* =========================================================================
 * Background Worker: Pack Directory -> VFS Container
 * ======================================================================= */
class PackWorker : public QObject {
    Q_OBJECT
  public:
    PackWorker(QString dirPath, QString containerPath, QObject* parent = nullptr);
    void cancel() { m_canceled.store(true); }

  public slots:
    void process();

  signals:
    void progress(int current, int total, const QString& currentFile);
    void finished(bool success, size_t fileCount, size_t errorCount, const QString& errorMsg);

  private:
    QString m_dirPath;
    QString m_containerPath;
    std::atomic<bool> m_canceled{false};
};

/* =========================================================================
 * Background Worker: Unpack VFS Container -> Host Directory
 * ======================================================================= */
class UnpackWorker : public QObject {
    Q_OBJECT
  public:
    UnpackWorker(QString containerPath, QString outDir, QObject* parent = nullptr);
    void cancel() { m_canceled.store(true); }

  public slots:
    void process();

  signals:
    void progress(int current, int total, const QString& currentFile);
    void finished(bool success, size_t fileCount, size_t errorCount, const QString& errorMsg);

  private:
    QString m_containerPath;
    QString m_outDir;
    std::atomic<bool> m_canceled{false};
};

/* =========================================================================
 * Main Window
 * ======================================================================= */
class MainWindow : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    void setVFSFile(const QString& path);

  private slots:
    // Container lifecycle & tools
    void onCreateVfs();
    void onOpenVfs();
    void onCloseVfs();
    void onPackDirectory();
    void onUnpackContainer();
    void onDumpSuperblock();

    // File manipulation
    void onAddFile();
    void onExtractFile();
    void onDeleteFile();
    void onRenameFile();
    void onRefresh();

    // UI interactions
    void onSelectionChanged();
    void onSearchChanged(const QString& text);
    void onTableItemDoubleClicked(int row, int column);
    void onContextMenuRequested(const QPoint& pos);

  private:
    void setupUi();
    void setupMenusAndToolbars();
    void closeVfs();
    void refreshList();
    void updateButtonsState();
    void updatePreviewPane(const QString& vfsPath);
    void clearPreviewPane();

    // Internal helpers
    bool importHostFile(const QString& hostPath, const QString& vfsPath);
    bool exportVfsFile(const QString& vfsPath, const QString& hostPath);
    static QString formatSize(uint64_t bytes);
    static QString formatHexDump(const QByteArray& data, int maxBytes = 16384);

    vfs_t* m_vfs{nullptr};
    QString m_currentImage;
    bool m_isOperationRunning{false};

    // Main UI controls
    QTableWidget* m_table;
    QLineEdit* m_searchEdit;
    QLabel* m_statusLabel;
    QLabel* m_storageInfoLabel;

    // Preview Pane widgets
    QTabWidget* m_previewTabs;
    QPlainTextEdit* m_textPreview;
    QLabel* m_imagePreviewLabel;
    QScrollArea* m_imageScrollArea;
    QPlainTextEdit* m_hexPreview;
    QPlainTextEdit* m_statInfoView;

    // Actions
    QAction* m_actNew;
    QAction* m_actOpen;
    QAction* m_actClose;
    QAction* m_actPack;
    QAction* m_actUnpack;
    QAction* m_actDump;
    QAction* m_actAdd;
    QAction* m_actExtract;
    QAction* m_actRename;
    QAction* m_actDelete;
    QAction* m_actRefresh;
};
