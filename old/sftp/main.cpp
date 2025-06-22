#include <QApplication>
#include <QMainWindow>
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
#include <QInputDialog>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QLabel>
#include <QStack>
#include <QDebug>

#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <iostream>
#include <vector>
#include <map>

class SSHSession {
public:
    ssh_session session = nullptr;
    sftp_session sftp = nullptr;

    bool connectToHost(const QString& host, const QString& user, const QString& password) {
        session = ssh_new();
        if (!session) return false;

        ssh_options_set(session, SSH_OPTIONS_HOST, host.toStdString().c_str());
        ssh_options_set(session, SSH_OPTIONS_USER, user.toStdString().c_str());
     //   ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &SSH_LOG_NOLOG);

        if (ssh_connect(session) != SSH_OK) {
            ssh_free(session);
            return false;
        }

        if (ssh_userauth_password(session, nullptr, password.toStdString().c_str()) != SSH_AUTH_SUCCESS) {
            ssh_disconnect(session);
            ssh_free(session);
            return false;
        }

        sftp = sftp_new(session);
        if (!sftp || sftp_init(sftp) != SSH_OK) {
            sftp_free(sftp);
            ssh_disconnect(session);
            ssh_free(session);
            return false;
        }

        return true;
    }

    std::vector<QString> listDir(const QString& path) {
        std::vector<QString> files;
        if (!sftp) return files;

        sftp_dir dir = sftp_opendir(sftp, path.toStdString().c_str());
        if (!dir) return files;

        sftp_attributes attr;
        while ((attr = sftp_readdir(sftp, dir))) {
            files.emplace_back(attr->name);
            sftp_attributes_free(attr);
        }

        sftp_closedir(dir);
        return files;
    }

    void disconnect() {
        if (sftp) sftp_free(sftp);
        if (session) {
            ssh_disconnect(session);
            ssh_free(session);
        }
        sftp = nullptr;
        session = nullptr;
    }

    ~SSHSession() { disconnect(); }
};

class FileBrowser : public QMainWindow {
    Q_OBJECT

    QListWidget* fileList;
    QLineEdit* pathEdit;
    QLineEdit* searchEdit;
    QPushButton* backBtn;
    QPushButton* fwdBtn;
    QPushButton* connectBtn;
    QPushButton* actionMenuBtn;
    QPlainTextEdit* console;
    QMenu* actionMenu;
    QProcess* process;
    SSHSession ssh;
    QStack<QString> backStack;
    QStack<QString> forwardStack;
    QString currentPath = "/";
    std::map<QString, std::function<void(QString)>> quickActions;

public:
    FileBrowser(QWidget* parent = nullptr) : QMainWindow(parent) {
        QWidget* central = new QWidget;
        QVBoxLayout* mainLayout = new QVBoxLayout;
        QHBoxLayout* navLayout = new QHBoxLayout;

        backBtn = new QPushButton("<");
        fwdBtn = new QPushButton(">");
        pathEdit = new QLineEdit("/");
        searchEdit = new QLineEdit();
        searchEdit->setPlaceholderText("Search...");
        connectBtn = new QPushButton("Connect");
        actionMenuBtn = new QPushButton("Quick Actions");

        navLayout->addWidget(backBtn);
        navLayout->addWidget(fwdBtn);
        navLayout->addWidget(pathEdit);
        navLayout->addWidget(searchEdit);
        navLayout->addWidget(connectBtn);
        navLayout->addWidget(actionMenuBtn);

        fileList = new QListWidget;
        console = new QPlainTextEdit;
        console->setReadOnly(true);

        actionMenu = new QMenu(this);
        actionMenuBtn->setMenu(actionMenu);

        process = new QProcess(this);
        connect(process, &QProcess::readyReadStandardOutput, this, [this]() {
            console->appendPlainText(process->readAllStandardOutput());
        });

        QSplitter* splitter = new QSplitter(Qt::Vertical);
        splitter->addWidget(fileList);
        splitter->addWidget(console);

        mainLayout->addLayout(navLayout);
        mainLayout->addWidget(splitter);
        central->setLayout(mainLayout);
        setCentralWidget(central);
        setWindowTitle("SSH File Browser");

        connect(connectBtn, &QPushButton::clicked, this, &FileBrowser::connectToServer);
        connect(backBtn, &QPushButton::clicked, this, &FileBrowser::goBack);
        connect(fwdBtn, &QPushButton::clicked, this, &FileBrowser::goForward);
        connect(pathEdit, &QLineEdit::returnPressed, this, &FileBrowser::browsePath);
        connect(fileList, &QListWidget::itemDoubleClicked, this, &FileBrowser::enterDirectory);
        connect(searchEdit, &QLineEdit::textChanged, this, &FileBrowser::filterFiles);

        actionMenu->addAction("Add Action", this, &FileBrowser::addQuickAction);
        actionMenu->addAction("Remove Action", this, &FileBrowser::removeQuickAction);
    }

    void connectToServer() {
        QString host = QInputDialog::getText(this, "Host", "Enter Host:");
        QString user = QInputDialog::getText(this, "Username", "Enter Username:");
        QString pass = QInputDialog::getText(this, "Password", "Enter Password:", QLineEdit::Password);

        if (!ssh.connectToHost(host, user, pass)) {
            QMessageBox::critical(this, "Error", "SSH Connection Failed");
            return;
        }

        loadDirectory("/");
    }

    void loadDirectory(const QString& path) {
        auto files = ssh.listDir(path);
        if (files.empty()) {
            QMessageBox::warning(this, "Warning", "Unable to list directory");
            return;
        }

        fileList->clear();
        for (const auto& f : files)
            fileList->addItem(f);

        backStack.push(currentPath);
        currentPath = path;
        pathEdit->setText(path);
    }

    void browsePath() {
        QString newPath = pathEdit->text();
        if (!newPath.isEmpty())
            loadDirectory(newPath);
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
        QString name = item->text();
        QString newPath = currentPath.endsWith("/") ? currentPath + name : currentPath + "/" + name;
        loadDirectory(newPath);
    }

    void filterFiles(const QString& text) {
        for (int i = 0; i < fileList->count(); ++i) {
            QListWidgetItem* item = fileList->item(i);
            item->setHidden(!item->text().contains(text, Qt::CaseInsensitive));
        }
    }

    void addQuickAction() {
        QString name = QInputDialog::getText(this, "Action Name", "Enter Action Name:");
        QString command = QInputDialog::getText(this, "Shell Command", "Enter Shell Command:");

        if (name.isEmpty() || command.isEmpty()) return;

        QAction* act = new QAction(name, this);
        connect(act, &QAction::triggered, this, [this, command]() {
            console->appendPlainText("Running: " + command);
            process->start(command);
        });

        quickActions[name] = [command](QString) {};
        actionMenu->addAction(act);
    }

    void removeQuickAction() {
        QString name = QInputDialog::getText(this, "Remove Action", "Enter Action Name to Remove:");
        QList<QAction*> acts = actionMenu->actions();
        for (QAction* act : acts) {
            if (act->text() == name) {
                actionMenu->removeAction(act);
                delete act;
                quickActions.erase(name);
                break;
            }
        }
    }

    ~FileBrowser() { ssh.disconnect(); }
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    FileBrowser fb;
    fb.resize(800, 600);
    fb.show();
    return app.exec();
}

#include "main.moc"
