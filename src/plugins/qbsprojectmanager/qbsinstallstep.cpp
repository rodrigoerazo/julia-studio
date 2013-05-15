/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "qbsinstallstep.h"

#include "qbsbuildconfiguration.h"
#include "qbsproject.h"
#include "qbsprojectmanagerconstants.h"

#include "ui_qbsinstallstepconfigwidget.h"

#include <projectexplorer/buildsteplist.h>
#include <projectexplorer/deployconfiguration.h>
#include <projectexplorer/kit.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/target.h>
#include <utils/qtcassert.h>

#include <QFileInfo>

static const char QBS_INSTALL_ROOT[] = "Qbs.InstallRoot";
static const char QBS_REMOVE_FIRST[] = "Qbs.RemoveFirst";
static const char QBS_DRY_RUN[] = "Qbs.DryRun";
static const char QBS_KEEP_GOING[] = "Qbs.DryKeepGoing";

// --------------------------------------------------------------------
// Constants:
// --------------------------------------------------------------------

namespace QbsProjectManager {
namespace Internal {

// --------------------------------------------------------------------
// QbsInstallStep:
// --------------------------------------------------------------------

QbsInstallStep::QbsInstallStep(ProjectExplorer::BuildStepList *bsl) :
    ProjectExplorer::BuildStep(bsl, Core::Id(Constants::QBS_INSTALLSTEP_ID)),
    m_job(0), m_showCompilerOutput(true), m_parser(0)
{
    setDisplayName(tr("Qbs Install"));
}

QbsInstallStep::QbsInstallStep(ProjectExplorer::BuildStepList *bsl, const QbsInstallStep *other) :
    ProjectExplorer::BuildStep(bsl, Core::Id(Constants::QBS_INSTALLSTEP_ID)),
    m_qbsInstallOptions(other->m_qbsInstallOptions), m_job(0),
    m_showCompilerOutput(other->m_showCompilerOutput), m_parser(0)
{ }

QbsInstallStep::~QbsInstallStep()
{
    cancel();
    if (m_job)
        m_job->deleteLater();
    m_job = 0;
}

bool QbsInstallStep::init()
{
    QTC_ASSERT(!static_cast<QbsProject *>(project())->isParsing() && !m_job, return false);
    return true;
}

void QbsInstallStep::run(QFutureInterface<bool> &fi)
{
    m_fi = &fi;

    QbsProject *pro = static_cast<QbsProject *>(project());
    m_job = pro->install(m_qbsInstallOptions);

    if (!m_job) {
        m_fi->reportResult(false);
        return;
    }

    m_progressBase = 0;

    connect(m_job, SIGNAL(finished(bool,qbs::AbstractJob*)), this, SLOT(installDone(bool)));
    connect(m_job, SIGNAL(taskStarted(QString,int,qbs::AbstractJob*)),
            this, SLOT(handleTaskStarted(QString,int)));
    connect(m_job, SIGNAL(taskProgress(int,qbs::AbstractJob*)),
            this, SLOT(handleProgress(int)));
}

ProjectExplorer::BuildStepConfigWidget *QbsInstallStep::createConfigWidget()
{
    return new QbsInstallStepConfigWidget(this);
}

bool QbsInstallStep::runInGuiThread() const
{
    return true;
}

void QbsInstallStep::cancel()
{
    if (m_job)
        m_job->cancel();
}

QString QbsInstallStep::installRoot() const
{
    if (!m_qbsInstallOptions.installRoot.isEmpty())
        return m_qbsInstallOptions.installRoot;

    return qbs::InstallOptions::defaultInstallRoot();
}

QString QbsInstallStep::absoluteInstallRoot() const
{
    const qbs::ProjectData *data = static_cast<QbsProject *>(project())->qbsProjectData();
    QString path = installRoot();
    if (data && !data->buildDirectory().isEmpty() && !path.isEmpty())
        path = QDir(data->buildDirectory()).absoluteFilePath(path);
    return path;
}

bool QbsInstallStep::removeFirst() const
{
    return m_qbsInstallOptions.removeFirst;
}

bool QbsInstallStep::dryRun() const
{
    return m_qbsInstallOptions.dryRun;
}

bool QbsInstallStep::keepGoing() const
{
    return m_qbsInstallOptions.keepGoing;
}

bool QbsInstallStep::fromMap(const QVariantMap &map)
{
    if (!ProjectExplorer::BuildStep::fromMap(map))
        return false;

    setInstallRoot(map.value(QLatin1String(QBS_INSTALL_ROOT)).toString());
    m_qbsInstallOptions.removeFirst = map.value(QLatin1String(QBS_REMOVE_FIRST), false).toBool();
    m_qbsInstallOptions.dryRun = map.value(QLatin1String(QBS_DRY_RUN), false).toBool();
    m_qbsInstallOptions.keepGoing = map.value(QLatin1String(QBS_KEEP_GOING), false).toBool();

    return true;
}

QVariantMap QbsInstallStep::toMap() const
{
    QVariantMap map = ProjectExplorer::BuildStep::toMap();
    map.insert(QLatin1String(QBS_INSTALL_ROOT), m_qbsInstallOptions.installRoot);
    map.insert(QLatin1String(QBS_REMOVE_FIRST), m_qbsInstallOptions.removeFirst);
    map.insert(QLatin1String(QBS_DRY_RUN), m_qbsInstallOptions.dryRun);
    map.insert(QLatin1String(QBS_KEEP_GOING), m_qbsInstallOptions.keepGoing);

    return map;
}

void QbsInstallStep::installDone(bool success)
{
    // Report errors:
    foreach (const qbs::ErrorData &data, m_job->error().entries()) {
        createTaskAndOutput(ProjectExplorer::Task::Error, data.description(),
                            data.codeLocation().fileName, data.codeLocation().line);
    }

    QTC_ASSERT(m_fi, return);
    m_fi->reportResult(success);
    m_fi = 0; // do not delete, it is not ours
    m_job->deleteLater();
    m_job = 0;

    emit finished();
}

void QbsInstallStep::handleTaskStarted(const QString &desciption, int max)
{
    Q_UNUSED(desciption);
    QTC_ASSERT(m_fi, return);
    m_progressBase = m_fi->progressValue();
    m_fi->setProgressRange(0, m_progressBase + max);
}

void QbsInstallStep::handleProgress(int value)
{
    QTC_ASSERT(m_fi, return);
    m_fi->setProgressValue(m_progressBase + value);
}

void QbsInstallStep::createTaskAndOutput(ProjectExplorer::Task::TaskType type,
                                         const QString &message, const QString &file, int line)
{
    emit addTask(ProjectExplorer::Task(type, message,
                                       Utils::FileName::fromString(file), line,
                                       ProjectExplorer::Constants::TASK_CATEGORY_COMPILE));
    emit addOutput(message, NormalOutput);
}

void QbsInstallStep::setInstallRoot(const QString &ir)
{
    if (m_qbsInstallOptions.installRoot == ir)
        return;
    m_qbsInstallOptions.installRoot = ir;
    emit changed();
}

void QbsInstallStep::setRemoveFirst(bool rf)
{
    if (m_qbsInstallOptions.removeFirst == rf)
        return;
    m_qbsInstallOptions.removeFirst = rf;
    emit changed();
}

void QbsInstallStep::setDryRun(bool dr)
{
    if (m_qbsInstallOptions.dryRun == dr)
        return;
    m_qbsInstallOptions.dryRun = dr;
    emit changed();
}

void QbsInstallStep::setKeepGoing(bool kg)
{
    if (m_qbsInstallOptions.keepGoing == kg)
        return;
    m_qbsInstallOptions.keepGoing = kg;
    emit changed();
}

// --------------------------------------------------------------------
// QbsInstallStepConfigWidget:
// --------------------------------------------------------------------

QbsInstallStepConfigWidget::QbsInstallStepConfigWidget(QbsInstallStep *step) :
    m_step(step), m_ignoreChange(false)
{
    connect(m_step, SIGNAL(displayNameChanged()), this, SLOT(updateState()));
    connect(m_step, SIGNAL(changed()), this, SLOT(updateState()));

    setContentsMargins(0, 0, 0, 0);

    QbsProject *project = static_cast<QbsProject *>(m_step->project());

    m_ui = new Ui::QbsInstallStepConfigWidget;
    m_ui->setupUi(this);

    m_ui->installRootChooser->setPromptDialogTitle(tr("Qbs Install Prefix"));
    m_ui->installRootChooser->setExpectedKind(Utils::PathChooser::Directory);

    connect(m_ui->installRootChooser, SIGNAL(changed(QString)), this, SLOT(changeInstallRoot()));
    connect(m_ui->removeFirstCheckBox, SIGNAL(toggled(bool)), this, SLOT(changeRemoveFirst(bool)));
    connect(m_ui->dryRunCheckBox, SIGNAL(toggled(bool)), this, SLOT(changeDryRun(bool)));
    connect(m_ui->keepGoingCheckBox, SIGNAL(toggled(bool)), this, SLOT(changeKeepGoing(bool)));

    connect(project, SIGNAL(projectParsingDone(bool)), this, SLOT(updateState()));

    updateState();
}

QString QbsInstallStepConfigWidget::summaryText() const
{
    return m_summary;
}

QString QbsInstallStepConfigWidget::displayName() const
{
    return m_step->displayName();
}

void QbsInstallStepConfigWidget::updateState()
{
    if (!m_ignoreChange) {
        m_ui->installRootChooser->setPath(m_step->installRoot());
        m_ui->removeFirstCheckBox->setChecked(m_step->removeFirst());
        m_ui->dryRunCheckBox->setChecked(m_step->dryRun());
        m_ui->keepGoingCheckBox->setChecked(m_step->keepGoing());
    }

    const qbs::ProjectData *data = static_cast<QbsProject *>(m_step->project())->qbsProjectData();
    if (data)
        m_ui->installRootChooser->setBaseDirectory(data->buildDirectory());

    QString command = QLatin1String("qbs install ");
    if (m_step->dryRun())
        command += QLatin1String("--dry-run ");
    if (m_step->keepGoing())
        command += QLatin1String("--keep-going ");
    if (m_step->removeFirst())
        command += QLatin1String("--remove-first ");
    command += QString::fromLatin1("--install-root \"%1\"").arg(m_step->absoluteInstallRoot());

    QString summary = tr("<b>Qbs:</b> %1").arg(command);
    if (m_summary != summary) {
        m_summary = summary;
        emit updateSummary();
    }
}

void QbsInstallStepConfigWidget::changeInstallRoot()
{
    const QString path = m_ui->installRootChooser->path();
    if (m_step->installRoot() == path)
        return;

    m_ignoreChange = true;
    m_step->setInstallRoot(path);
    m_ignoreChange = false;
}

void QbsInstallStepConfigWidget::changeRemoveFirst(bool rf)
{
    m_step->setRemoveFirst(rf);
}

void QbsInstallStepConfigWidget::changeDryRun(bool dr)
{
    m_step->setDryRun(dr);
}

void QbsInstallStepConfigWidget::changeKeepGoing(bool kg)
{
    m_step->setKeepGoing(kg);
}

// --------------------------------------------------------------------
// QbsInstallStepFactory:
// --------------------------------------------------------------------

QbsInstallStepFactory::QbsInstallStepFactory(QObject *parent) :
    ProjectExplorer::IBuildStepFactory(parent)
{ }

QList<Core::Id> QbsInstallStepFactory::availableCreationIds(ProjectExplorer::BuildStepList *parent) const
{
    if (parent->id() == ProjectExplorer::Constants::BUILDSTEPS_DEPLOY
            && qobject_cast<ProjectExplorer::DeployConfiguration *>(parent->parent()))
        return QList<Core::Id>() << Core::Id(Constants::QBS_INSTALLSTEP_ID);
    return QList<Core::Id>();
}

QString QbsInstallStepFactory::displayNameForId(const Core::Id id) const
{
    if (id == Core::Id(Constants::QBS_INSTALLSTEP_ID))
        return tr("Qbs Install");
    return QString();
}

bool QbsInstallStepFactory::canCreate(ProjectExplorer::BuildStepList *parent, const Core::Id id) const
{
    if (parent->id() != Core::Id(ProjectExplorer::Constants::BUILDSTEPS_DEPLOY)
            || !qobject_cast<ProjectExplorer::DeployConfiguration *>(parent->parent()))
        return false;
    return id == Core::Id(Constants::QBS_INSTALLSTEP_ID);
}

ProjectExplorer::BuildStep *QbsInstallStepFactory::create(ProjectExplorer::BuildStepList *parent,
                                                          const Core::Id id)
{
    if (!canCreate(parent, id))
        return 0;
    return new QbsInstallStep(parent);
}

bool QbsInstallStepFactory::canRestore(ProjectExplorer::BuildStepList *parent, const QVariantMap &map) const
{
    return canCreate(parent, ProjectExplorer::idFromMap(map));
}

ProjectExplorer::BuildStep *QbsInstallStepFactory::restore(ProjectExplorer::BuildStepList *parent,
                                                           const QVariantMap &map)
{
    if (!canRestore(parent, map))
        return 0;
    QbsInstallStep *bs = new QbsInstallStep(parent);
    if (!bs->fromMap(map)) {
        delete bs;
        return 0;
    }
    return bs;
}

bool QbsInstallStepFactory::canClone(ProjectExplorer::BuildStepList *parent,
                                     ProjectExplorer::BuildStep *product) const
{
    return canCreate(parent, product->id());
}

ProjectExplorer::BuildStep *QbsInstallStepFactory::clone(ProjectExplorer::BuildStepList *parent,
                                                         ProjectExplorer::BuildStep *product)
{
    if (!canClone(parent, product))
        return 0;
    return new QbsInstallStep(parent, static_cast<QbsInstallStep *>(product));
}

} // namespace Internal
} // namespace QbsProjectManager
