// ssh_browser.cpp - Qt 5.12 SSH File Browser (no SFTP)
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
#include <QProcess>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QLabel>
#include <QStack>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QSettings>
#include <QInputDialog>
#include <libssh/libssh.h>
#include <libssh/callbacks.h>
#include <iostream>
#include <map>

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

        char buffer[256];
        int nbytes;
        while ((nbytes = ssh_channel_read(channel, buffer, sizeof(buffer), 0)) > 0) {
            output.append(QString::fromUtf8(buffer, nbytes).split('\n'));
        }

        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);

        return output;
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

class ConnectionManager : public QDialog {
    Q_OBJECT

public:
    QLineEdit *hostEdit, *userEdit, *passEdit;
    QPushButton* connectBtn;

    ConnectionManager(QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle("SSH Connection Manager");
        QVBoxLayout* layout = new QVBoxLayout;

        hostEdit = new QLineEdit; hostEdit->setPlaceholderText("Host");
        userEdit = new QLineEdit; userEdit->setPlaceholderText("Username");
        passEdit = new QLineEdit; passEdit->setPlaceholderText("Password"); passEdit->setEchoMode(QLineEdit::Password);
        connectBtn = new QPushButton("Connect");

        layout->addWidget(new QLabel("Host:")); layout->addWidget(hostEdit);
        layout->addWidget(new QLabel("Username:")); layout->addWidget(userEdit);
        layout->addWidget(new QLabel("Password:")); layout->addWidget(passEdit);
        layout->addWidget(connectBtn);
        setLayout(layout);

        connect(connectBtn, &QPushButton::clicked, this, &ConnectionManager::accept);
    }

    QString getHost() const { return hostEdit->text(); }
    QString getUser() const { return userEdit->text(); }
    QString getPass() const { return passEdit->text(); }
};

class FileBrowser : public QMainWindow {
    Q_OBJECT

    QListWidget* fileList;
    QLineEdit* pathEdit, *searchEdit;
    QPushButton* backBtn, *fwdBtn, *connectBtn, *actionMenuBtn;
    QPlainTextEdit* console;
    QMenu* actionMenu;
    QFileIconProvider iconProvider;
    QStack<QString> backStack, forwardStack;
    QString currentPath = "/";
    QProcess* process;
    SSHSession ssh;
    std::map<QString, QString> quickActions;

public:
    FileBrowser() {
        QWidget* central = new QWidget;
        QVBoxLayout* mainLayout = new QVBoxLayout;
        QHBoxLayout* topLayout = new QHBoxLayout;

        backBtn = new QPushButton("<");
        fwdBtn = new QPushButton(">");
        pathEdit = new QLineEdit("/");
        searchEdit = new QLineEdit(); searchEdit->setPlaceholderText("Search...");
        connectBtn = new QPushButton("Connect");
        actionMenuBtn = new QPushButton("Actions");

        topLayout->addWidget(backBtn);
        topLayout->addWidget(fwdBtn);
        topLayout->addWidget(pathEdit);
        topLayout->addWidget(searchEdit);
        topLayout->addWidget(connectBtn);
        topLayout->addWidget(actionMenuBtn);

        fileList = new QListWidget;
        console = new QPlainTextEdit; console->setReadOnly(true);
        QSplitter* splitter = new QSplitter(Qt::Vertical);
        splitter->addWidget(fileList);
        splitter->addWidget(console);

        mainLayout->addLayout(topLayout);
        mainLayout->addWidget(splitter);
        central->setLayout(mainLayout);
        setCentralWidget(central);
        setWindowTitle("SSH File Browser");

        actionMenu = new QMenu(this);
        actionMenuBtn->setMenu(actionMenu);
        actionMenu->addAction("Add Action", this, &FileBrowser::addQuickAction);
        actionMenu->addAction("Remove Action", this, &FileBrowser::removeQuickAction);

        process = new QProcess(this);
        connect(process, &QProcess::readyReadStandardOutput, this, [this]() {
            console->appendPlainText(process->readAllStandardOutput());
        });

        connect(backBtn, &QPushButton::clicked, this, &FileBrowser::goBack);
        connect(fwdBtn, &QPushButton::clicked, this, &FileBrowser::goForward);
        connect(pathEdit, &QLineEdit::returnPressed, this, &FileBrowser::browseToPath);
        connect(connectBtn, &QPushButton::clicked, this, &FileBrowser::showConnectionDialog);
        connect(fileList, &QListWidget::itemDoubleClicked, this, &FileBrowser::enterDirectory);
        connect(searchEdit, &QLineEdit::textChanged, this, &FileBrowser::filterFiles);
    }

    void showConnectionDialog() {
        ConnectionManager dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            if (ssh.connectToHost(dlg.getHost(), dlg.getUser(), dlg.getPass())) {
                loadDirectory("/");
            } else {
                QMessageBox::critical(this, "Connection Failed", "Could not connect to server");
            }
        }
    }

    void loadDirectory(const QString& path) {
        QStringList output = ssh.runCommand("ls -p \"" + path + "\"");
        if (output.isEmpty()) {
            QMessageBox::warning(this, "Error", "Could not list directory.");
            return;
        }

        fileList->clear();
        for (const QString& entry : output) {
            if (entry.trimmed().isEmpty()) continue;
            QListWidgetItem* item = new QListWidgetItem(entry);
            bool isDir = entry.endsWith("/");
            item->setIcon(iconProvider.icon(isDir ? QFileIconProvider::Folder : QFileIconProvider::File));
            item->setData(Qt::UserRole, isDir);
            fileList->addItem(item);
        }

        backStack.push(currentPath);
        currentPath = path;
        pathEdit->setText(path);
    }

    void browseToPath() {
        loadDirectory(pathEdit->text().trimmed());
    }

    void goBack() {
        if (!backStack.isEmpty()) {
            forwardStack.push(currentPath);
            QString prev = backStack.pop();
            loadDirectory(prev);
        }
    }

    void goForward() {
        if (!forwardStack.isEmpty()) {
            backStack.push(currentPath);
            QString next = forwardStack.pop();
            loadDirectory(next);
        }
    }

    void enterDirectory(QListWidgetItem* item) {
        if (!item->data(Qt::UserRole).toBool()) return;
        QString name = item->text();
        QString path = currentPath.endsWith("/") ? currentPath + name : currentPath + "/" + name;
        loadDirectory(path);
    }

    void filterFiles(const QString& term) {
        for (int i = 0; i < fileList->count(); ++i) {
            QListWidgetItem* item = fileList->item(i);
            item->setHidden(!item->text().contains(term, Qt::CaseInsensitive));
        }
    }

    void addQuickAction() {
        QString name = QInputDialog::getText(this, "Action Name", "Enter Action Name:");
        QString command = QInputDialog::getText(this, "SSH Command", "Command to execute:");
        if (name.isEmpty() || command.isEmpty()) return;

        QAction* act = new QAction(name, this);
        connect(act, &QAction::triggered, this, [this, command]() {
            QStringList out = ssh.runCommand(command);
            for (const QString& line : out) console->appendPlainText(line);
        });

        quickActions[name] = command;
        actionMenu->addAction(act);
    }

    void removeQuickAction() {
        QString name = QInputDialog::getText(this, "Remove Action", "Name of Action:");
        for (QAction* act : actionMenu->actions()) {
            if (act->text() == name) {
                actionMenu->removeAction(act);
                quickActions.erase(name);
                delete act;
                break;
            }
        }
    }

    ~FileBrowser() { ssh.disconnect(); }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    FileBrowser browser;
    browser.resize(800, 600);
    browser.show();
    return app.exec();
}

#include "main.moc"
