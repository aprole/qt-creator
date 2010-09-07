/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2010 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://qt.nokia.com/contact.
**
**************************************************************************/

#include "abstractprocessstep.h"
#include "buildconfiguration.h"
#include "buildstep.h"
#include "ioutputparser.h"
#include "project.h"
#include "target.h"

#include <utils/qtcassert.h>

#include <QtCore/QProcess>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtGui/QTextDocument>

using namespace ProjectExplorer;

AbstractProcessStep::AbstractProcessStep(BuildStepList *bsl, const QString &id) :
    BuildStep(bsl, id), m_timer(0), m_futureInterface(0),
    m_enabled(true), m_ignoreReturnValue(false),
    m_process(0), m_eventLoop(0), m_outputParserChain(0)
{
}

AbstractProcessStep::AbstractProcessStep(BuildStepList *bsl,
                                         AbstractProcessStep *bs) :
    BuildStep(bsl, bs), m_timer(0), m_futureInterface(0),
    m_enabled(bs->m_enabled), m_ignoreReturnValue(bs->m_ignoreReturnValue),
    m_process(0), m_eventLoop(0), m_outputParserChain(0)
{
}

AbstractProcessStep::~AbstractProcessStep()
{
    delete m_process;
    delete m_timer;
    // do not delete m_futureInterface, we do not own it.
    delete m_outputParserChain;
}

void AbstractProcessStep::setCommand(const QString &cmd)
{
    m_command = cmd;
}

QString AbstractProcessStep::workingDirectory() const
{
    return m_workingDirectory;
}

void AbstractProcessStep::setOutputParser(ProjectExplorer::IOutputParser *parser)
{
    delete m_outputParserChain;
    m_outputParserChain = parser;

    if (m_outputParserChain) {
        connect(parser, SIGNAL(addOutput(QString, ProjectExplorer::BuildStep::OutputFormat)),
                this, SLOT(outputAdded(QString, ProjectExplorer::BuildStep::OutputFormat)));
        connect(parser, SIGNAL(addTask(ProjectExplorer::Task)),
                this, SLOT(taskAdded(ProjectExplorer::Task)));
    }
}

void AbstractProcessStep::appendOutputParser(ProjectExplorer::IOutputParser *parser)
{
    if (!parser)
        return;

    QTC_ASSERT(m_outputParserChain, return);
    m_outputParserChain->appendOutputParser(parser);
    return;
}

ProjectExplorer::IOutputParser *AbstractProcessStep::outputParser() const
{
    return m_outputParserChain;
}

void AbstractProcessStep::setWorkingDirectory(const QString &workingDirectory)
{
    m_workingDirectory = workingDirectory;
}

void AbstractProcessStep::setArguments(const QStringList &arguments)
{
    m_arguments = arguments;
}

void AbstractProcessStep::setEnabled(bool b)
{
    m_enabled = b;
}

void AbstractProcessStep::setIgnoreReturnValue(bool b)
{
    m_ignoreReturnValue = b;
}

void AbstractProcessStep::setEnvironment(Environment env)
{
    m_environment = env;
}

bool AbstractProcessStep::init()
{
    if (QFileInfo(m_command).isRelative()) {
        QString searchInPath = m_environment.searchInPath(m_command);
        if (!searchInPath.isEmpty())
            m_command = searchInPath;
    }
    return true;
}

void AbstractProcessStep::run(QFutureInterface<bool> &fi)
{
    m_futureInterface = &fi;
    if (!m_enabled) {
        fi.reportResult(true);
        return;
    }
    QDir wd(m_workingDirectory);
    if (!wd.exists())
        wd.mkpath(wd.absolutePath());

    m_process = new QProcess();
    m_process->setWorkingDirectory(m_workingDirectory);
    m_process->setEnvironment(m_environment.toStringList());

    connect(m_process, SIGNAL(readyReadStandardOutput()),
            this, SLOT(processReadyReadStdOutput()),
            Qt::DirectConnection);
    connect(m_process, SIGNAL(readyReadStandardError()),
            this, SLOT(processReadyReadStdError()),
            Qt::DirectConnection);

    connect(m_process, SIGNAL(finished(int, QProcess::ExitStatus)),
            this, SLOT(slotProcessFinished(int, QProcess::ExitStatus)),
            Qt::DirectConnection);

    m_process->start(m_command, m_arguments);
    if (!m_process->waitForStarted()) {
        processStartupFailed();
        delete m_process;
        m_process = 0;
        fi.reportResult(false);
        return;
    }
    processStarted();

    m_timer = new QTimer();
    connect(m_timer, SIGNAL(timeout()), this, SLOT(checkForCancel()), Qt::DirectConnection);
    m_timer->start(500);
    m_eventLoop = new QEventLoop;
    m_eventLoop->exec();
    m_timer->stop();
    delete m_timer;
    m_timer = 0;

    // The process has finished, leftover data is read in processFinished
    processFinished(m_process->exitCode(), m_process->exitStatus());
    bool returnValue = processSucceeded(m_process->exitCode(), m_process->exitStatus()) || m_ignoreReturnValue;

    // Clean up output parsers
    if (m_outputParserChain) {
        delete m_outputParserChain;
        m_outputParserChain = 0;
    }

    delete m_process;
    m_process = 0;
    delete m_eventLoop;
    m_eventLoop = 0;
    fi.reportResult(returnValue);
    m_futureInterface = 0;
    return;
}

void AbstractProcessStep::processStarted()
{
    emit addOutput(tr("Starting: \"%1\" %2\n").arg(QDir::toNativeSeparators(m_command), m_arguments.join(" ")), BuildStep::MessageOutput);
}

void AbstractProcessStep::processFinished(int exitCode, QProcess::ExitStatus status)
{
    if (status == QProcess::NormalExit && exitCode == 0) {
        emit addOutput(tr("The process \"%1\" exited normally.")
                       .arg(QDir::toNativeSeparators(m_command)),
                       BuildStep::MessageOutput);
    } else if (status == QProcess::NormalExit) {
        emit addOutput(tr("The process \"%1\" exited with code %2.")
                       .arg(QDir::toNativeSeparators(m_command), QString::number(m_process->exitCode())),
                       BuildStep::ErrorMessageOutput);
    } else {
        emit addOutput(tr("The process \"%1\" crashed.").arg(QDir::toNativeSeparators(m_command)), BuildStep::ErrorMessageOutput);
    }
}

void AbstractProcessStep::processStartupFailed()
{
    emit addOutput(tr("Could not start process \"%1\"").arg(QDir::toNativeSeparators(m_command)), BuildStep::ErrorMessageOutput);
}

bool AbstractProcessStep::processSucceeded(int exitCode, QProcess::ExitStatus status)
{
    return exitCode == 0 && status == QProcess::NormalExit;
}

void AbstractProcessStep::processReadyReadStdOutput()
{
    m_process->setReadChannel(QProcess::StandardOutput);
    while (m_process->canReadLine()) {
        QString line = QString::fromLocal8Bit(m_process->readLine());
        stdOutput(line);
    }
}

void AbstractProcessStep::stdOutput(const QString &line)
{
    if (m_outputParserChain)
        m_outputParserChain->stdOutput(line);
    emit addOutput(line, BuildStep::NormalOutput);
}

void AbstractProcessStep::processReadyReadStdError()
{
    m_process->setReadChannel(QProcess::StandardError);
    while (m_process->canReadLine()) {
        QString line = QString::fromLocal8Bit(m_process->readLine());
        stdError(line);
    }
}

void AbstractProcessStep::stdError(const QString &line)
{
    if (m_outputParserChain)
        m_outputParserChain->stdError(line);
    emit addOutput(line, BuildStep::ErrorOutput);
}

void AbstractProcessStep::checkForCancel()
{
    if (m_futureInterface->isCanceled() && m_timer->isActive()) {
        m_timer->stop();
        m_process->terminate();
        m_process->waitForFinished(5000);
        m_process->kill();
    }
}

void AbstractProcessStep::taskAdded(const ProjectExplorer::Task &task)
{
    // Do not bother to report issues if we do not care about the results of
    // the buildstep anyway:
    if (m_ignoreReturnValue)
        return;

    Task editable(task);
    QString filePath = QDir::cleanPath(task.file.trimmed());
    if (!filePath.isEmpty() && !QDir::isAbsolutePath(filePath)) {
        // We have no save way to decide which file in which subfolder
        // is meant. Therefore we apply following heuristics:
        // 1. Check if file is unique in whole project
        // 2. Otherwise try again without any ../
        // 3. give up.

        QList<QFileInfo> possibleFiles;
        QString fileName = QFileInfo(filePath).fileName();
        foreach (const QString &file, buildConfiguration()->target()->project()->files(ProjectExplorer::Project::AllFiles)) {
            QFileInfo candidate(file);
            if (candidate.fileName() == fileName)
                possibleFiles << candidate;
        }

        if (possibleFiles.count() == 1) {
            editable.file = possibleFiles.first().filePath();
        } else {
            // More then one filename, so do a better compare
            // Chop of any "../"
            while (filePath.startsWith(QLatin1String("../")))
                filePath.remove(0, 3);
            int count = 0;
            QString possibleFilePath;
            foreach(const QFileInfo &fi, possibleFiles) {
                if (fi.filePath().endsWith(filePath)) {
                    possibleFilePath = fi.filePath();
                    ++count;
                }
            }
            if (count == 1)
                editable.file = possibleFilePath;
            else
                qWarning() << "Could not find absolute location of file " << filePath;
        }
    }
    emit addTask(editable);
}

void AbstractProcessStep::outputAdded(const QString &string, ProjectExplorer::BuildStep::OutputFormat format)
{
    emit addOutput(string, format);
}

void AbstractProcessStep::slotProcessFinished(int, QProcess::ExitStatus)
{
    QString line = QString::fromLocal8Bit(m_process->readAllStandardError());
    if (!line.isEmpty())
        stdError(line);

    line = QString::fromLocal8Bit(m_process->readAllStandardOutput());
    if (!line.isEmpty())
        stdOutput(line);

    m_eventLoop->exit(0);
}
