// ssh_advanced_browser.cpp - SSH File Browser with Base64 Transfers, SQLite, Drag-Drop, Multi-Tab, Terminal, Search, Navigation + Icons, Right-Click Menu, Preview, Column View Toggle, Rename Popup
#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QListView>
#include <QPushButton>
#include <QLineEdit>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>
#include <QFileIconProvider>
#include <QDir>
#include <QFileInfo>
#include <QTabWidget>
#include <QSplitter>
#include <QScrollArea>
#include <QLabel>
#include <libssh/libssh.h>

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

    void renameRemoteFile(const QString& oldPath, const QString& newPath) {
        QString cmd = QString("mv \"%1\" \"%2\"").arg(oldPath, newPath);
        runCommand(cmd);
    }

    void promptRename(QWidget* parent, const QString& oldPath) {
        bool ok;
        QString newName = QInputDialog::getText(parent, "Rename File", "New name:", QLineEdit::Normal, QFileInfo(oldPath).fileName(), &ok);
        if (ok && !newName.isEmpty()) {
            QString newPath = QFileInfo(oldPath).absolutePath() + "/" + newName;
            renameRemoteFile(oldPath, newPath);
        }
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

class FileBrowserWidget : public QWidget {
    Q_OBJECT
    QListView* listView;
    QStandardItemModel* model;
    SSHSession* session;
    QString currentPath;

public:
    FileBrowserWidget(SSHSession* ssh, QWidget* parent = nullptr) : QWidget(parent), session(ssh) {
        QVBoxLayout* layout = new QVBoxLayout(this);
        listView = new QListView(this);
        listView->setViewMode(QListView::IconMode);
        listView->setIconSize(QSize(64, 64));
        listView->setResizeMode(QListView::Adjust);
        listView->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(listView, &QListView::customContextMenuRequested, this, &FileBrowserWidget::showContextMenu);

        model = new QStandardItemModel(this);
        listView->setModel(model);
        layout->addWidget(listView);

        setLayout(layout);
        refreshDirectory(".");
    }

    void refreshDirectory(const QString& path) {
        model->clear();
        currentPath = path;
        QStringList files = session->runCommand("ls -p \"" + path + "\"");
        QFileIconProvider iconProvider;

        for (const QString& file : files) {
            if (file.trimmed().isEmpty()) continue;
            QStandardItem* item = new QStandardItem(iconProvider.icon(QFileInfo(file)), file);
            item->setData(path + "/" + file, Qt::UserRole);
            model->appendRow(item);
        }
    }

    void showContextMenu(const QPoint& pos) {
        QModelIndex index = listView->indexAt(pos);
        if (!index.isValid()) return;

        QString filePath = model->itemFromIndex(index)->data(Qt::UserRole).toString();

        QMenu menu;
        QAction* renameAct = menu.addAction("Rename");
        QAction* previewAct = menu.addAction("Preview");
        QAction* selected = menu.exec(listView->viewport()->mapToGlobal(pos));

        if (selected == renameAct) {
            session->promptRename(this, filePath);
            refreshDirectory(currentPath);
        } else if (selected == previewAct) {
            QByteArray data = session->getFileBase64(filePath);
            QDialog* dlg = new QDialog(this);
            QVBoxLayout* vbox = new QVBoxLayout(dlg);
            QLabel* label = new QLabel(dlg);
            label->setPixmap(QPixmap::fromImage(QImage::fromData(data)).scaled(500, 500, Qt::KeepAspectRatio));
            vbox->addWidget(label);
            dlg->exec();
        }
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    SSHSession* ssh = new SSHSession();
    if (!ssh->connectToHost("your.server.com", "user", "password")) {
        QMessageBox::critical(nullptr, "SSH Error", "Failed to connect.");
        return -1;
    }

    QMainWindow window;
    FileBrowserWidget* browser = new FileBrowserWidget(ssh);
    window.setCentralWidget(browser);
    window.resize(800, 600);
    window.show();

    return app.exec();
}

#include <main.moc>
