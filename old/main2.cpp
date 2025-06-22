// ssh_advanced_browser.cpp - SSH File Browser with Base64 Transfers, SQLite, Drag-Drop, Multi-Tab
#include <QApplication>
#include <QMainWindow>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QComboBox>
#include <QMenu>
#include <QAction>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QLabel>
#include <QStack>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QSettings>
#include <QInputDialog>
#include <QMessageBox>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QPixmap>
#include <QDialogButtonBox>
#include <QTextEdit>
#include <QBuffer>
#include <QByteArray>
#include <QTextStream>
#include <QImageReader>
#include <QDialog>
#include <QFileDialog>
#include <QTabWidget>
#include <QDropEvent>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <libssh/libssh.h>
#include <fstream>
#include <qmenubar.h>

class SSHSession {
public:
    ssh_session session = nullptr;

    bool connectToHost(const QString& host, const QString& user, const QString& password) {
        session = ssh_new();
        if (!session) return false;
        ssh_options_set(session, SSH_OPTIONS_HOST, host.toStdString().c_str());
        ssh_options_set(session, SSH_OPTIONS_USER, user.toStdString().c_str());

        if (ssh_connect(session) != SSH_OK)
            return false;

        if (ssh_userauth_password(session, nullptr, password.toStdString().c_str()) != SSH_AUTH_SUCCESS)
            return false;

        return true;
    }

    QStringList runCommand(const QString& cmd) {
        QStringList output;
        if (!session) return output;

        ssh_channel channel = ssh_channel_new(session);
        if (!channel) return output;

        if (ssh_channel_open_session(channel) != SSH_OK) return output;
        if (ssh_channel_request_exec(channel, cmd.toStdString().c_str()) != SSH_OK) {
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            return output;
        }

        char buffer[4096];
        int nbytes;
        while ((nbytes = ssh_channel_read(channel, buffer, sizeof(buffer), 0)) > 0) {
            output.append(QString::fromUtf8(buffer, nbytes).split('\n'));
        }

        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);

        return output;
    }

    QByteArray getFileBase64(const QString& path) {
        QByteArray result;
        if (!session) return result;

        ssh_channel channel = ssh_channel_new(session);
        if (!channel) return result;

        if (ssh_channel_open_session(channel) != SSH_OK) return result;
        QString cmd = "base64 \"" + path + "\"";
        if (ssh_channel_request_exec(channel, cmd.toStdString().c_str()) != SSH_OK) {
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            return result;
        }

        char buffer[4096];
        int nbytes;
        while ((nbytes = ssh_channel_read(channel, buffer, sizeof(buffer), 0)) > 0) {
            result.append(QByteArray(buffer, nbytes));
        }

        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);

        return QByteArray::fromBase64(result);
    }

    void uploadFileBase64(const QString& localPath, const QString& remotePath) {
        QFile file(localPath);
        if (!file.open(QIODevice::ReadOnly)) return;

        QByteArray base64Data = file.readAll().toBase64();
        QString echoCmd = QString("echo '%1' | base64 -d > \"%2\"")
                          .arg(QString::fromUtf8(base64Data))
                          .arg(remotePath);
        runCommand(echoCmd);
    }

    void disconnect() {
        if (session) {
            ssh_disconnect(session);
            ssh_free(session);
            session = nullptr;
        }
    }

    ~SSHSession() { disconnect(); }
};

class FilePreviewDialog : public QDialog {
public:
    FilePreviewDialog(const QByteArray& data, const QString& name, QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle("Preview: " + name);
        QVBoxLayout* layout = new QVBoxLayout(this);

        QImage img;
        if (img.loadFromData(data)) {
            QLabel* imageLabel = new QLabel;
            imageLabel->setPixmap(QPixmap::fromImage(img));
            layout->addWidget(imageLabel);
        } else {
            QTextEdit* textEdit = new QTextEdit;
            textEdit->setReadOnly(true);
            textEdit->setPlainText(QString::fromUtf8(data));
            layout->addWidget(textEdit);
        }
    }
};

class FileTab : public QWidget {
    Q_OBJECT
public:
    FileTab(QWidget* parent = nullptr) : QWidget(parent) {
        // Placeholder: actual tab content injected in main UI
    }
};

class FileBrowser : public QMainWindow {
    Q_OBJECT
    QTabWidget* tabWidget;
    QSqlDatabase db;

public:
    FileBrowser() {
        setWindowTitle("SSH File Browser - Multi-Session");
        resize(1000, 700);

        tabWidget = new QTabWidget(this);
        setCentralWidget(tabWidget);

        QMenu* fileMenu = menuBar()->addMenu("&File");
        QAction* newSession = fileMenu->addAction("New Session");
        connect(newSession, &QAction::triggered, this, &FileBrowser::newConnectionTab);

        QMenu* actionsMenu = menuBar()->addMenu("&Actions");
        actionsMenu->addAction("Manage Actions", [this]() {
           QMessageBox::information(this, "Actions", "(To be implemented)");
        });

        initDB();
        newConnectionTab();
    }

    void initDB() {
        db = QSqlDatabase::addDatabase("QSQLITE");
        db.setDatabaseName("settings.db");
        db.open();
        QSqlQuery q;
        q.exec("CREATE TABLE IF NOT EXISTS connections (host TEXT, user TEXT, pass TEXT)");
        q.exec("CREATE TABLE IF NOT EXISTS actions (name TEXT, command TEXT)");
    }

    void newConnectionTab() {
        // TODO: Load from connection manager UI
        QString host = QInputDialog::getText(this, "Host", "Enter host:");
        QString user = QInputDialog::getText(this, "User", "Enter username:");
        QString pass = QInputDialog::getText(this, "Pass", "Enter password:", QLineEdit::Password);

        QWidget* sessionTab = new QWidget;
        QVBoxLayout* layout = new QVBoxLayout(sessionTab);

        QLineEdit* pathEdit = new QLineEdit("/");
        QPlainTextEdit* console = new QPlainTextEdit; console->setReadOnly(true);
        QListWidget* fileList = new QListWidget;

        layout->addWidget(pathEdit);
        layout->addWidget(fileList);
        layout->addWidget(console);

        sessionTab->setAcceptDrops(true);
        sessionTab->setLayout(layout);

        SSHSession* session = new SSHSession;
        if (!session->connectToHost(host, user, pass)) {
            delete session;
            delete sessionTab;
            QMessageBox::critical(this, "Error", "Connection failed");
            return;
        }

        auto loadDir = [=](const QString& path) {
            fileList->clear();
            pathEdit->setText(path);
            QStringList out = session->runCommand("ls -p \"" + path + "\"");
            for (const QString& entry : out) {
                QListWidgetItem* item = new QListWidgetItem(entry);
                bool isDir = entry.endsWith("/");
                item->setData(Qt::UserRole, isDir);
                item->setIcon(QFileIconProvider().icon(isDir ? QFileIconProvider::Folder : QFileIconProvider::File));
                fileList->addItem(item);
            }
        };

        connect(pathEdit, &QLineEdit::returnPressed, [=]() { loadDir(pathEdit->text()); });
        connect(fileList, &QListWidget::itemDoubleClicked, [=](QListWidgetItem* item) {
            if (item->data(Qt::UserRole).toBool()) {
                QString next = pathEdit->text();
                if (!next.endsWith("/")) next += "/";
                loadDir(next + item->text());
            } else {
                QString full = pathEdit->text() + (pathEdit->text().endsWith("/") ? "" : "/") + item->text();
                QByteArray data = session->getFileBase64(full);
                FilePreviewDialog* dlg = new FilePreviewDialog(data, item->text(), this);
                dlg->exec();
            }
        });

        sessionTab->installEventFilter(this);
        tabWidget->addTab(sessionTab, host);
        loadDir("/");
    }

    bool eventFilter(QObject* obj, QEvent* event) override {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            QDragEnterEvent* drag = static_cast<QDragEnterEvent*>(event);
            drag->acceptProposedAction();
            return true;
        }
        if (event->type() == QEvent::Drop) {
            QDropEvent* drop = static_cast<QDropEvent*>(event);
            const QMimeData* mime = drop->mimeData();
            if (mime->hasUrls()) {
                QString path = QInputDialog::getText(this, "Remote Path", "Upload to:", QLineEdit::Normal, "/");
                QWidget* tab = tabWidget->currentWidget();
                SSHSession* session = new SSHSession; // Replace with per-tab instance
                for (const QUrl& url : mime->urls()) {
                    session->uploadFileBase64(url.toLocalFile(), path + "/" + QFileInfo(url.toLocalFile()).fileName());
                }
            }
            return true;
        }
        return QMainWindow::eventFilter(obj, event);
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    FileBrowser browser;
    browser.show();
    return app.exec();
}

#include "main.moc"
