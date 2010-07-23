/****************************************************************************
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** This file is part of the Qt Creator.
**
** $QT_BEGIN_LICENSE:LGPL$
** No Commercial Usage
** This file contains pre-release code and may not be distributed.
** You may use this file in accordance with the terms and conditions
** contained in the Technology Preview License Agreement accompanying
** this package.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights.  These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** If you have questions regarding the use of this file, please contact
** Nokia at qt-info@nokia.com.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "maemosshrunner.h"

#include "maemodeviceconfigurations.h"
#include "maemoglobal.h"
#include "maemoremotemountsmodel.h"
#include "maemorunconfiguration.h"
#include "maemotoolchain.h"

#include <coreplugin/ssh/sftpchannel.h>
#include <coreplugin/ssh/sshconnection.h>
#include <coreplugin/ssh/sshremoteprocess.h>

#include <QtCore/QFileInfo>
#include <QtCore/QProcess>

using namespace Core;

namespace Qt4ProjectManager {
namespace Internal {

MaemoSshRunner::MaemoSshRunner(QObject *parent,
    MaemoRunConfiguration *runConfig)
    : QObject(parent), m_runConfig(runConfig),
      m_devConfig(runConfig->deviceConfig()),
      m_uploadJobId(SftpInvalidJob)
{
    m_procsToKill
        << QFileInfo(m_runConfig->localExecutableFilePath()).fileName()
        << QLatin1String("utfs-client");
}

MaemoSshRunner::~MaemoSshRunner() {}

void MaemoSshRunner::setConnection(const QSharedPointer<Core::SshConnection> &connection)
{
    m_connection = connection;
}

void MaemoSshRunner::addProcsToKill(const QStringList &appNames)
{
    m_procsToKill << appNames;
}

void MaemoSshRunner::start()
{
    m_stop = false;
    if (m_connection)
    disconnect(m_connection.data(), 0, this, 0);
    const bool reUse = m_connection
        && m_connection->state() == SshConnection::Connected
        && m_connection->connectionParameters() == m_devConfig.server;
    if (!reUse)
        m_connection = SshConnection::create();
    connect(m_connection.data(), SIGNAL(connected()), this,
        SLOT(handleConnected()));
    connect(m_connection.data(), SIGNAL(error(SshError)), this,
        SLOT(handleConnectionFailure()));
    if (reUse)
        handleConnected();
    else
        m_connection->connectToHost(m_devConfig.server);
}

void MaemoSshRunner::stop()
{
    m_stop = true;
    disconnect(m_connection.data(), 0, this, 0);
    if (m_initialCleaner)
        disconnect(m_initialCleaner.data(), 0, this, 0);
    if (m_utfsClientUploader) {
        disconnect(m_utfsClientUploader.data(), 0, this, 0);
        m_utfsClientUploader->closeChannel();
    }
    if (m_mountProcess) {
        disconnect(m_mountProcess.data(), 0, this, 0);
        m_mountProcess->closeChannel();
    }
    if (m_runner) {
        disconnect(m_runner.data(), 0, this, 0);
        m_runner->closeChannel();
        cleanup(false);
    }
}

void MaemoSshRunner::handleConnected()
{
    if (m_stop)
        return;

    cleanup(true);
}

void MaemoSshRunner::handleConnectionFailure()
{
    emit error(tr("Could not connect to host: %1")
        .arg(m_connection->errorString()));
}

void MaemoSshRunner::cleanup(bool initialCleanup)
{
    QString niceKill;
    QString brutalKill;
    foreach (const QString &proc, m_procsToKill) {
        niceKill += QString::fromLocal8Bit("pkill -x %1;").arg(proc);
        brutalKill += QString::fromLocal8Bit("pkill -x -9 %1;").arg(proc);
    }
    QString remoteCall = niceKill + QLatin1String("sleep 1; ") + brutalKill;

    const MaemoRemoteMountsModel * const remoteMounts
        = m_runConfig->remoteMounts();
    for (int i = 0; i < remoteMounts->mountSpecificationCount(); ++i) {
        remoteCall += QString::fromLocal8Bit("%1 umount %2;")
            .arg(MaemoGlobal::remoteSudo(),
                 remoteMounts->mountSpecificationAt(i).remoteMountPoint);
    }

    remoteCall.remove(remoteCall.count() - 1, 1); // Get rid of trailing semicolon.
    SshRemoteProcess::Ptr proc
        = m_connection->createRemoteProcess(remoteCall.toUtf8());
    if (initialCleanup) {
        m_initialCleaner = proc;
        connect(m_initialCleaner.data(), SIGNAL(closed(int)), this,
            SLOT(handleInitialCleanupFinished(int)));
    }
    proc->start();
}

void MaemoSshRunner::handleInitialCleanupFinished(int exitStatus)
{
    Q_ASSERT(exitStatus == SshRemoteProcess::FailedToStart
        || exitStatus == SshRemoteProcess::KilledBySignal
        || exitStatus == SshRemoteProcess::ExitedNormally);

    if (m_stop)
        return;

    foreach (QProcess *utfsServer, m_utfsServers) {
        utfsServer->terminate();
        utfsServer->waitForFinished(1000);
        utfsServer->kill();
    }
    qDeleteAll(m_utfsServers);
    if (exitStatus != SshRemoteProcess::ExitedNormally) {
        emit error(tr("Initial cleanup failed: %1")
            .arg(m_initialCleaner->errorString()));
    } else if (m_runConfig->remoteMounts()->mountSpecificationCount() != 0) {
        deployUtfsClient();
    } else {
        emit readyForExecution();
    }
}

void MaemoSshRunner::deployUtfsClient()
{
    m_utfsClientUploader = m_connection->createSftpChannel();
    connect(m_utfsClientUploader.data(), SIGNAL(initialized()), this,
        SLOT(handleUploaderInitialized()));
    connect(m_utfsClientUploader.data(), SIGNAL(initializationFailed(QString)),
        this, SLOT(handleUploaderInitializationFailed(QString)));
    m_utfsClientUploader->initialize();
}

void MaemoSshRunner::handleUploaderInitializationFailed(const QString &reason)
{
    if (m_stop)
        return;

    emit error(tr("Failed to establish SFTP connection: %1").arg(reason));
}

void MaemoSshRunner::handleUploaderInitialized()
{
    if (m_stop)
        return;

    connect(m_utfsClientUploader.data(),
        SIGNAL(finished(Core::SftpJobId, QString)), this,
        SLOT(handleUploadFinished(Core::SftpJobId,QString)));
    const MaemoToolChain * const toolChain
        = dynamic_cast<const MaemoToolChain *>(m_runConfig->toolchain());
    Q_ASSERT_X(toolChain, Q_FUNC_INFO,
        "Impossible: Maemo run configuration has no Maemo Toolchain.");
    const QString localFile
        = toolChain->maddeRoot() + QLatin1String("/madlib/utfs-client");
    m_uploadJobId
        = m_utfsClientUploader->uploadFile(localFile, utfsClientOnDevice(),
              SftpOverwriteExisting);
    if (m_uploadJobId == SftpInvalidJob)
        emit error(tr("Could not upload UTFS client (%1).").arg(localFile));
}

void MaemoSshRunner::handleUploadFinished(Core::SftpJobId jobId,
    const QString &errorMsg)
{
    if (m_stop)
        return;

    if (jobId != m_uploadJobId) {
        qWarning("Warning: unknown upload job %d finished.", jobId);
        return;
    }

    m_uploadJobId = SftpInvalidJob;
    if (!errorMsg.isEmpty()) {
        emit error(tr("Could not upload UTFS client: %1").arg(errorMsg));
        return;
    }

    mount();
}

void MaemoSshRunner::mount()
{
    const MaemoRemoteMountsModel * const remoteMounts
        = m_runConfig->remoteMounts();
    QString remoteCall(QLatin1String(":"));
    for (int i = 0; i < remoteMounts->mountSpecificationCount(); ++i) {
        const MaemoRemoteMountsModel::MountSpecification &mountSpec
            = remoteMounts->mountSpecificationAt(i);

        QProcess * const utfsServerProc = new QProcess(this);
        const QString port = QString::number(mountSpec.port);
        const QString localSecretOpt = QLatin1String("-l");
        const QString remoteSecretOpt = QLatin1String("-r");
        const QStringList utfsServerArgs = QStringList() << localSecretOpt
            << port << remoteSecretOpt << port << QLatin1String("-b") << port;
        utfsServerProc->start(utfsServer(), utfsServerArgs);
        if (!utfsServerProc->waitForStarted()) {
            emit error(tr("Could not start UTFS server: %1")
                .arg(utfsServerProc->errorString()));
            return;
        }
        m_utfsServers << utfsServerProc;
        const QString mkdir = QString::fromLocal8Bit("%1 mkdir -p %2")
            .arg(MaemoGlobal::remoteSudo(), mountSpec.remoteMountPoint);
        const QString utfsClient
            = QString::fromLocal8Bit("%1 -l %2 -r %2 -c %3:%2 %4")
                  .arg(utfsClientOnDevice()).arg(port)
                  .arg(m_runConfig->localHostAddressFromDevice())
                  .arg(mountSpec.remoteMountPoint);
        const QLatin1String andOp(" && ");
        remoteCall += andOp + mkdir + andOp + utfsClient;
    }

    m_mountProcess = m_connection->createRemoteProcess(remoteCall.toUtf8());
    connect(m_mountProcess.data(), SIGNAL(started()), this,
        SLOT(handleMountProcessStarted()));
    connect(m_mountProcess.data(), SIGNAL(closed(int)), this,
        SLOT(handleRemoteProcessFinished(int)));
    m_mountProcess->start();
}

void MaemoSshRunner::handleMountProcessStarted()
{
    // TODO: Do we get "finished" from utfs-client when it goes into background?
    // If so, remnove this slot; readyForExecution() should be emitted from
    // handleRemoteProcessFinished() in that case.
    if (!m_stop)
        emit readyForExecution();
}

void MaemoSshRunner::handleMountProcessFinished(int exitStatus)
{
    if (m_stop)
        return;

    if (exitStatus != SshRemoteProcess::ExitedNormally) {
        emit error(tr("Failure running UTFS client: %1")
            .arg(m_mountProcess->errorString()));
    }
}

void MaemoSshRunner::startExecution(const QByteArray &remoteCall)
{
    if (m_runConfig->remoteExecutableFilePath().isEmpty()) {
        emit error(tr("Cannot run: No remote executable set."));
        return;
    }

    m_runner = m_connection->createRemoteProcess(remoteCall);
    connect(m_runner.data(), SIGNAL(started()), this,
        SIGNAL(remoteProcessStarted()));
    connect(m_runner.data(), SIGNAL(closed(int)), this,
        SLOT(handleRemoteProcessFinished(int)));
    connect(m_runner.data(), SIGNAL(outputAvailable(QByteArray)), this,
        SIGNAL(remoteOutput(QByteArray)));
    connect(m_runner.data(), SIGNAL(errorOutputAvailable(QByteArray)), this,
        SIGNAL(remoteErrorOutput(QByteArray)));
    m_runner->start();
}

void MaemoSshRunner::handleRemoteProcessFinished(int exitStatus)
{
    Q_ASSERT(exitStatus == SshRemoteProcess::FailedToStart
        || exitStatus == SshRemoteProcess::KilledBySignal
        || exitStatus == SshRemoteProcess::ExitedNormally);

    if (m_stop)
        return;

    if (exitStatus == SshRemoteProcess::ExitedNormally) {
        emit remoteProcessFinished(m_runner->exitCode());
    } else {
        emit error(tr("Error running remote process: %1")
            .arg(m_runner->errorString()));
    }
}

QString MaemoSshRunner::utfsClientOnDevice() const
{
    return MaemoGlobal::homeDirOnDevice(QLatin1String("developer"))
        + QLatin1String("/utfs-client");
}

QString MaemoSshRunner::utfsServer() const
{
    const MaemoToolChain * const toolChain
        = dynamic_cast<const MaemoToolChain *>(m_runConfig->toolchain());
    Q_ASSERT_X(toolChain, Q_FUNC_INFO,
        "Impossible: Maemo run configuration has no Maemo Toolchain.");
    return toolChain->maddeRoot() + QLatin1String("/madlib/utfs-server");
}

} // namespace Internal
} // namespace Qt4ProjectManager

