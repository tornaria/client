#pragma once

#include "httpcredentials.h"

#include "account.h"
#include "configfile.h"
#include "theme.h"

#include <QApplication>
#include <QLoggingCategory>

#include <qt5keychain/keychain.h>

namespace {
constexpr int CredentialVersion = 1;
auto CredentialVersionKey()
{
    return QStringLiteral("CredentialVersion");
}

const QString userC()
{
    return QStringLiteral("user");
}
const char authenticationFailedC[] = "owncloud-authentication-failed";

void addSettingsToJob(QKeychain::Job *job)
{
    auto settings = OCC::ConfigFile::settingsWithGroup(OCC::Theme::instance()->appName());
    settings->setParent(job); // make the job parent to make setting deleted properly
    job->setSettings(settings.release());
}

} // ns

Q_DECLARE_LOGGING_CATEGORY(lcHttpLegacyCredentials)
namespace OCC {

class HttpLegacyCredentials : public QObject
{
    Q_OBJECT

    const QString clientCertBundleC()
    {
        return QStringLiteral("clientCertPkcs12");
    }
    const QString clientCertPasswordC()
    {
        return QStringLiteral("_clientCertPassword");
    }
    const QString clientCertificatePEMC()
    {
        return QStringLiteral("_clientCertificatePEM");
    }
    const QString clientKeyPEMC()
    {
        return QStringLiteral("_clientKeyPEM");
    }

    auto isOAuthC()
    {
        return QStringLiteral("oauth");
    }

public:
    HttpLegacyCredentials(HttpCredentials *parent)
        : QObject(parent)
        , _parent(parent)
    {
    }

    void fetchFromKeychainHelper()
    {
        _parent->_clientCertBundle = _parent->_account->credentialSetting(clientCertBundleC()).toByteArray();

        if (!_parent->_clientCertBundle.isEmpty()) {
            // New case (>=2.6): We have a bundle in the settings and read the password from
            // the keychain
            QKeychain::ReadPasswordJob *job = new QKeychain::ReadPasswordJob(Theme::instance()->appName());
            job->setKey(_parent->keychainKey(_parent->_account->url().toString(), _parent->_user + clientCertPasswordC(), _parent->_account->id()));
            job->setInsecureFallback(false);
            connect(job, &QKeychain::ReadPasswordJob::finished, this, &HttpLegacyCredentials::slotReadClientCertPasswordJobDone);
            job->start();
            return;
        }
        // Old case (pre 2.6): Read client cert and then key from keychain
        const QString kck = _parent->keychainKey(
            _parent->_account->url().toString(),
            _parent->_user + clientCertificatePEMC(),
            _keychainMigration ? QString() : _parent->_account->id());

        QKeychain::ReadPasswordJob *job = new QKeychain::ReadPasswordJob(Theme::instance()->appName());
        addSettingsToJob(job);
        job->setInsecureFallback(false);
        job->setKey(kck);
        connect(job, &QKeychain::ReadPasswordJob::finished, this, [job] {
            if (job->error() == QKeychain::NoError && !job->binaryData().isEmpty()) {
                const auto msg = tr("The support of client side certificate saved in the keychain was removed, please start the setup wizard again and follow the instructions.");
                QMetaObject::invokeMethod(qApp, "slotShowGuiMessage", Qt::QueuedConnection, Q_ARG(QString, tr("Credentials")), Q_ARG(QString, msg));
                qCWarning(lcHttpLegacyCredentials) << msg;
            }
        });
        job->start();

        slotReadPasswordFromKeychain();
    }

private:
    HttpCredentials *_parent;
    bool _retryOnKeyChainError = true;

    bool _keychainMigration = false;

    bool keychainUnavailableRetryLater(QKeychain::ReadPasswordJob *incoming)
    {
#if defined(Q_OS_UNIX) && !defined(Q_OS_MAC)
        Q_ASSERT(!incoming->insecureFallback()); // If insecureFallback is set, the next test would be pointless
        if (_retryOnKeyChainError && (incoming->error() == QKeychain::NoBackendAvailable || incoming->error() == QKeychain::OtherError)) {
            // Could be that the backend was not yet available. Wait some extra seconds.
            // (Issues #4274 and #6522)
            // (For kwallet, the error is OtherError instead of NoBackendAvailable, maybe a bug in QtKeychain)
            qCInfo(lcHttpLegacyCredentials) << "Backend unavailable (yet?) Retrying in a few seconds." << incoming->errorString();
            QTimer::singleShot(10000, this, &HttpLegacyCredentials::fetchFromKeychainHelper);
            _retryOnKeyChainError = false;
            return true;
        }
#else
        Q_UNUSED(incoming);
#endif
        _retryOnKeyChainError = false;
        return false;
    }

    void slotReadClientCertPasswordJobDone(QKeychain::Job *job)
    {
        auto readJob = qobject_cast<QKeychain::ReadPasswordJob *>(job);
        if (keychainUnavailableRetryLater(readJob))
            return;

        if (readJob->error() == QKeychain::NoError) {
            _parent->_clientCertPassword = readJob->binaryData();
        } else {
            qCWarning(lcHttpLegacyCredentials) << "Could not retrieve client cert password from keychain" << readJob->errorString();
        }

        if (!_parent->unpackClientCertBundle(_parent->_clientCertPassword)) {
            qCWarning(lcHttpLegacyCredentials) << "Could not unpack client cert bundle";
        }

        // don't clear the password, we are migrating the old settings
        // _parent->_clientCertPassword.clear();
        // _parent->_clientCertBundle.clear();

        slotReadPasswordFromKeychain();
    }

    void slotReadPasswordFromKeychain()
    {
        const QString kck = _parent->keychainKey(
            _parent->_account->url().toString(),
            _parent->_user,
            _keychainMigration ? QString() : _parent->_account->id());

        QKeychain::ReadPasswordJob *job = new QKeychain::ReadPasswordJob(Theme::instance()->appName());
        addSettingsToJob(job);
        job->setInsecureFallback(false);
        job->setKey(kck);
        connect(job, &QKeychain::ReadPasswordJob::finished, this, &HttpLegacyCredentials::slotReadJobDone);
        job->start();
    }


    void slotReadJobDone(QKeychain::Job *incoming)
    {
        auto job = qobject_cast<QKeychain::ReadPasswordJob *>(incoming);
        QKeychain::Error error = job->error();

        // If we can't find the credentials at the keys that include the account id,
        // try to read them from the legacy locations that don't have a account id.
        if (!_keychainMigration && error == QKeychain::EntryNotFound) {
            qCWarning(lcHttpLegacyCredentials)
                << "Could not find keychain entries, attempting to read from legacy locations";
            _keychainMigration = true;
            fetchFromKeychainHelper();
            return;
        }

        bool isOauth = _parent->_account->credentialSetting(isOAuthC()).toBool();
        if (isOauth) {
            _parent->_refreshToken = job->textData();
        } else {
            _parent->_password = job->textData();
        }

        if (_parent->_user.isEmpty()) {
            qCWarning(lcHttpLegacyCredentials) << "Strange: User is empty!";
        }

        if (!_parent->_refreshToken.isEmpty() && error == QKeychain::NoError) {
            _parent->refreshAccessToken();
        } else if (!_parent->_password.isEmpty() && error == QKeychain::NoError) {
            // All cool, the keychain did not come back with error.
            // Still, the password can be empty which indicates a problem and
            // the password dialog has to be opened.
            _parent->_ready = true;
            emit _parent->fetched();
        } else {
            // we come here if the password is empty or any other keychain
            // error happend.

            _parent->_fetchErrorString = job->error() != QKeychain::EntryNotFound ? job->errorString() : QString();

            _parent->_password = QString();
            _parent->_ready = false;
            emit _parent->fetched();
        }

        // If keychain data was read from legacy location, wipe these entries and store new ones
        deleteOldKeychainEntries();
        if (_parent->_ready) {
            _parent->persist();
            qCWarning(lcHttpLegacyCredentials) << "Migrated old keychain entries";
        }
        deleteLater();
    }

    void slotWriteJobDone(QKeychain::Job *job)
    {
        if (job && job->error() != QKeychain::NoError) {
            qCWarning(lcHttpLegacyCredentials) << "Error while writing password"
                                               << job->error() << job->errorString();
        }
    }

    void slotWriteClientCertPasswordJobDone(QKeychain::Job *finishedJob)
    {
        if (finishedJob && finishedJob->error() != QKeychain::NoError) {
            qCWarning(lcHttpLegacyCredentials) << "Could not write client cert password to credentials"
                                               << finishedJob->error() << finishedJob->errorString();
        }
    }

    void deleteOldKeychainEntries()
    {
        // oooold
        auto startDeleteJob = [this](const QString &key, bool migratePreId = false) {
            QKeychain::DeletePasswordJob *job = new QKeychain::DeletePasswordJob(Theme::instance()->appName());
            addSettingsToJob(job);
            job->setInsecureFallback(true);
            job->setKey(_parent->keychainKey(_parent->_account->url().toString(), key, migratePreId ? QString() : _parent->_account->id()));
            job->start();
            connect(job, &QKeychain::DeletePasswordJob::finished, this, [job] {
                if (job->error() != QKeychain::NoError) {
                    qCWarning(lcHttpLegacyCredentials) << "Failed to delete legacy credentials" << job->key() << job->errorString();
                }
            });
        };

        // old
        startDeleteJob(_parent->_user, true);
        startDeleteJob(_parent->_user + clientKeyPEMC(), true);
        startDeleteJob(_parent->_user + clientCertificatePEMC(), true);

        // pre 2.8
        startDeleteJob(_parent->_user + clientCertPasswordC());
        startDeleteJob(_parent->_user + clientKeyPEMC());
        startDeleteJob(_parent->_user);
    }
};

}
