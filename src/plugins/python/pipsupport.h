// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <utils/filepath.h>
#include <utils/qtcprocess.h>

#include <QFutureWatcher>
#include <QTimer>
#include <QUrl>

namespace Python::Internal {

class PipPackageInfo
{
public:
    QString name;
    QString version;
    QString summary;
    QUrl homePage;
    QString author;
    QString authorEmail;
    QString license;
    Utils::FilePath location;
    QStringList requiresPackage;
    QStringList requiredByPackage;
    Utils::FilePaths files;

    void parseField(const QString &field, const QStringList &value);
};

class PipPackage
{
public:
    explicit PipPackage(const QString &packageName = {},
                        const QString &displayName = {},
                        const QString &version = {})
        : packageName(packageName)
        , displayName(displayName.isEmpty() ? packageName : displayName)
        , version(version)
    {}
    QString packageName;
    QString displayName;
    QString version;
};

class Pip : public QObject
{
public:
    static Pip *instance(const Utils::FilePath &python);

    QFuture<PipPackageInfo> info(const PipPackage &package);

private:
    Pip(const Utils::FilePath &python);

    PipPackageInfo infoImpl(const PipPackage &package);

    QMutex m_lock;
    Utils::FilePath m_python;
};

class PipInstallTask : public QObject
{
    Q_OBJECT
public:
    explicit PipInstallTask(const Utils::FilePath &python);
    void setPackage(const PipPackage &package);
    void run();

signals:
    void finished(bool success);

private:
    void cancel();
    void handleDone();
    void handleOutput();
    void handleError();

    const Utils::FilePath m_python;
    PipPackage m_package;
    Utils::QtcProcess m_process;
    QFutureInterface<void> m_future;
    QFutureWatcher<void> m_watcher;
    QTimer m_killTimer;
};

} // Python::Internal
