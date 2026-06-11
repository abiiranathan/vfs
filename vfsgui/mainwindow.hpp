#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QTableWidget>

// Prevent C++ name mangling of our C-based VFS symbols
extern "C" {
#include <vfs.h>
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

  public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    void setVFSFile(const QString& path);

  private slots:
    void onCreateVfs();
    void onOpenVfs();
    void onAddFile();
    void onExtractFile();
    void onDeleteFile();
    void onRenameFile();
    void onRefresh();
    void onSelectionChanged();
    void onSearchChanged(const QString& text);

  private:
    void setupUi();
    void closeVfs();
    void refreshList();
    void updateButtonsState();

    vfs_t* m_vfs;
    QString m_currentImage;

    // UI Widgets
    QTableWidget* m_table;
    QLabel* m_statusLabel;
    QLabel* m_imageLabel;
    QLineEdit* m_searchEdit;

    QPushButton* m_btnOpen;
    QPushButton* m_btnCreate;
    QPushButton* m_btnAdd;
    QPushButton* m_btnExtract;
    QPushButton* m_btnRename;
    QPushButton* m_btnDelete;
    QPushButton* m_btnRefresh;
};
