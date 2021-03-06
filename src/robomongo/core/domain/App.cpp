#include "robomongo/core/domain/App.h"

#include <QHash>
#include <QInputDialog>
#include <QMessageBox>

#include "robomongo/core/domain/MongoServer.h"
#include "robomongo/core/domain/MongoShell.h"
#include "robomongo/core/domain/MongoCollection.h"
#include "robomongo/core/settings/ConnectionSettings.h"
#include "robomongo/core/settings/ReplicaSetSettings.h"
#include "robomongo/core/settings/SshSettings.h"
#include "robomongo/core/settings/SslSettings.h"
#include "robomongo/core/mongodb/SshTunnelWorker.h"
#include "robomongo/core/EventBus.h"
#include "robomongo/core/utils/QtUtils.h"
#include "robomongo/core/utils/StdUtils.h"
#include "robomongo/core/utils/Logger.h"

namespace Robomongo
{
    namespace detail
    {
        QString buildCollectionQuery(const std::string &collectionName, const QString &postfix)
        {
            QString qCollectionName = QtUtils::toQString(collectionName);

            QString pattern;

            // Use db.getCollection() to avoid having to enumerate and special case "reserved" names
            pattern = "db.getCollection(\'%1\').%2";

            // Escape '\' symbol
            qCollectionName.replace(QChar('\\'), "\\\\");

            return pattern.arg(qCollectionName).arg(postfix);
        }
    }

    App::~App()
    {}

    App::App(EventBus *const bus) : QObject(),
        _bus(bus), _lastServerHandle(0) {
        _bus->subscribe(this, EstablishSshConnectionResponse::Type);
        _bus->subscribe(this, ListenSshConnectionResponse::Type);
        _bus->subscribe(this, LogEvent::Type);
    }

    std::unique_ptr<MongoServer>
    App::continueOpenServer(int serverHandle, ConnectionSettings* connSettings, 
                            ConnectionType type, int localport)
    {
        ConnectionSettings* connSettingsClone = connSettings->clone();

        // Modify connection settings when SSH tunnel is used
        if ((type == ConnectionPrimary || type == ConnectionTest)
            && !connSettingsClone->isReplicaSet()
            && connSettingsClone->sshSettings()->enabled()
        ) {
            connSettingsClone->setServerHost("127.0.0.1");
            connSettingsClone->setServerPort(localport);
        }

        auto server { std::make_unique<MongoServer>(serverHandle, connSettingsClone, type) };
        server->runWorkerThread();

        auto replicaSetStr = QString::fromStdString(connSettings->connectionName()) + " [Replica Set]";
        replicaSetStr = (connSettings->replicaSetSettings()->members().size() > 0)
            ? replicaSetStr + QString::fromStdString(connSettings->replicaSetSettings()->members()[0])
            : replicaSetStr + "";

        QString serverAddress = connSettings->isReplicaSet()
            ? replicaSetStr
            : QString::fromStdString(connSettings->getFullAddress());

        LOG_MSG(QString("Connecting to %1...").arg(serverAddress), mongo::logger::LogSeverity::Info());
        server->tryConnect();
        return server;
    }

    /**
    * Creates and opens new server connection.
    * @param connection: ConnectionSettings, that will be owned by MongoServer.
    * @param visible: should this server be visible in UI (explorer) or not.
    */
    std::unique_ptr<MongoServer> 
    App::openServerInternal(ConnectionSettings* connSettings, ConnectionType type) 
    {
        ++_lastServerHandle;

        if (type == ConnectionPrimary)
            _bus->publish(new ConnectingEvent(this));

        // When connection is SECONDARY or SSH not enabled or replica set,
        // then continue without SSH Tunnel
        if (type == ConnectionSecondary || !connSettings->sshSettings()->enabled() 
            || connSettings->isReplicaSet() 
        ) {
            return continueOpenServer(_lastServerHandle, connSettings, type);
        }

        // Open SSH channel and only after that open connection
        LOG_MSG(QString("Creating SSH tunnel to %1:%2...")
            .arg(QtUtils::toQString(connSettings->sshSettings()->host()))
            .arg(connSettings->sshSettings()->port()), mongo::logger::LogSeverity::Info());

        ConnectionSettings* settingsCopy = connSettings->clone();
        SshTunnelWorker* sshWorker = new SshTunnelWorker(settingsCopy);
        _bus->send(sshWorker, new EstablishSshConnectionRequest(this, _lastServerHandle, sshWorker, settingsCopy, type));
        return nullptr;
    }

    bool App::openServer(ConnectionSettings *connection, ConnectionType type) 
    {
        SshSettings *ssh = connection->sshSettings();

        if (!connection->isReplicaSet() && ssh->enabled() && ssh->askPassword() &&
            (type == ConnectionPrimary || type == ConnectionTest)) {
            bool ok = false;

            bool isByKey = ssh->authMethod() == "publickey";
            std::string passText = isByKey ? "passphrase" : "password";

            std::stringstream s;
            s << "In order to continue, please provide the " << passText;

            if (isByKey)
                s << " for the key file";

            s << "." << std::endl << std::endl;

            if (ssh->authMethod() == "publickey")
                s << "Private Key:  " << ssh->privateKeyFile() << std::endl;

            s << "Server: " << ssh->host() << std::endl;
            s << "User: " << ssh->userName() << std::endl;


            s << std::endl << "Enter your " << passText << " that will never be stored:";

            QString userInput = QInputDialog::getText(NULL, tr("SSH Authentication"),
                QtUtils::toQString(s.str()),
                QLineEdit::Password, "", &ok);

            if (!ok)
                return false;

            ssh->setAskedPassword(QtUtils::toStdString(userInput));
        }

        SslSettings *sslSettings = connection->sslSettings();

        if (sslSettings->sslEnabled() && sslSettings->usePemFile() && sslSettings->askPassphrase() 
            && (type == ConnectionPrimary || type == ConnectionTest)) 
        {
            if (!askSslPassphrasePromptDialog(connection))
            {
                return false;
            }
        }

        _servers.push_back(move(openServerInternal(connection, type)));
        return true;
    }

    /**
     * @brief Closes MongoServer connection and frees all resources, owned
     * by MongoServer. Finally, specified MongoServer will also be deleted.
     */
    void App::closeServer(MongoServer *server)
    {
        _servers.erase(std::remove_if(_servers.begin(), _servers.end(), 
            [&](auto const& el) { return el.get() == server; }), _servers.end());
    }

    void App::openShell(MongoCollection *collection, const QString &filePathToSave)
    {
        ConnectionSettings *connection = collection->database()->server()->connectionRecord();
        auto const& dbname = collection->database()->name();
        connection->setDefaultDatabase(dbname);
        QString const& script = detail::buildCollectionQuery(collection->name(), "find({})");
        openShell(collection->database()->server(), connection, 
            ScriptInfo(script, true, dbname, CursorPosition(0, -2), QtUtils::toQString(dbname), filePathToSave)
        );
    }

    void App::openShell(MongoServer *server, const QString &script, const std::string &dbName,
                        bool execute, const QString &shellName,
                        const CursorPosition &cursorPosition, const QString &filePathToSave)
    {
        ConnectionSettings *connection = server->connectionRecord();

        if (!dbName.empty())
            connection->setDefaultDatabase(dbName);

        openShell(server, connection, 
            ScriptInfo(script, execute, dbName, cursorPosition, shellName, filePathToSave)
        );
    }

    void App::openShell(MongoDatabase *database, const QString &script,
                               bool execute, const QString &shellName,
                               const CursorPosition &cursorPosition, const QString &filePathToSave)
    {
        ConnectionSettings *connection = database->server()->connectionRecord();
        connection->setDefaultDatabase(database->name());
        openShell(database->server(), connection, ScriptInfo(script, execute, database->name(), 
                                                             cursorPosition, shellName, filePathToSave));
    }

    void App::openShell(MongoServer* server, ConnectionSettings* connection, const ScriptInfo &scriptInfo)
    {
        auto serverClone{ openServerInternal(connection, ConnectionSecondary) };
        if (!serverClone || !server)
            return;

        auto shell{ std::make_unique<MongoShell>(serverClone.get(), scriptInfo) };
        _servers.push_back(move(serverClone));
        // Connection between explorer's server and tab's MongoShells
        _bus->subscribe(server, ReplicaSetRefreshed::Type, shell.get()); 
        _bus->publish(new OpeningShellEvent(this, shell.get()));
        shell->execute();
        _shells.push_back(move(shell));
        return;
    }

    /**
     * @brief Closes MongoShell and frees all resources, owned by specified MongoShell.
     * Finally, specified MongoShell will also be deleted.
     */
    void App::closeShell(MongoShell *shell)
    {
        auto const itr = std::find_if(_shells.begin(), _shells.end(), 
            [&](auto const& el) { return el.get() == shell; }
        );
        
        // Do nothing, if this shell not owned by this App.
        if (itr == _shells.end())
            return;

        closeServer(shell->server());
        _shells.erase(itr);
    }

    void App::handle(EstablishSshConnectionResponse *event) {
        if (event->isError()) {
            _bus->publish(new ConnectionFailedEvent(
                this, event->serverHandle, event->connectionType, event->error().errorMessage(),
                ConnectionFailedEvent::SshConnection));
            return;
        }

        LOG_MSG(QString("SSH tunnel created successfully"), mongo::logger::LogSeverity::Info());

        _servers.push_back(move(
            continueOpenServer(event->serverHandle, event->settings, event->connectionType, event->localport)
        ));
        _bus->send(event->worker, new ListenSshConnectionRequest(this, event->serverHandle, event->connectionType));
    }

    void App::handle(LogEvent *event) {
        LOG_MSG(event->message, event->mongoLogSeverity());

        if (!event->informUser)
            return;

        QMessageBox(
            event->qMessageBoxIcon(),
            QString::fromStdString(event->severity()),
            QtUtils::toQString(event->severity() + ": " + event->message)
        ).exec();
    }

    void App::handle(ListenSshConnectionResponse *event) {
        if (event->isError()) {
            _bus->publish(
                new ConnectionFailedEvent(this, event->serverHandle, event->connectionType, 
                    event->error().errorMessage(), ConnectionFailedEvent::SshChannel)
            );
            return;
        }

        LOG_MSG(QString("SSH tunnel closed."), mongo::logger::LogSeverity::Error());
    }

    void App::fireConnectionFailedEvent(int serverHandle, ConnectionType type, std::string errormsg,
                                        ConnectionFailedEvent::Reason reason) {
        _bus->publish(new ConnectionFailedEvent(this, serverHandle, type, errormsg, reason));
    }

    bool App::askSslPassphrasePromptDialog(ConnectionSettings *connSettings) const
    {
        auto sslSettings = connSettings->sslSettings();
        bool ok = false;

        std::stringstream s;
        s << "In order to continue, please provide the passphrase";
        s << "." << std::endl << std::endl;

        s << "Server: " << connSettings->serverHost() << ":" << connSettings->serverPort() << std::endl;
        s << "PEM file: " << sslSettings->pemKeyFile() << std::endl;

        s << std::endl << "Enter your PEM key passphrase (will never be stored):";

        QString userInput = QInputDialog::getText(NULL, tr("TLS Authentication"),
            QtUtils::toQString(s.str()),
            QLineEdit::Password, "", &ok);

        if (!ok)
        {
            return false;
        }

        sslSettings->setPemPassPhrase(QtUtils::toStdString(userInput));
        return ok;
    }

}
