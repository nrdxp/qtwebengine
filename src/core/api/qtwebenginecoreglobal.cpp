/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtWebEngine module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qtwebenginecoreglobal_p.h"

#include <QGuiApplication>
#if QT_CONFIG(opengl)
# include <QOpenGLContext>
#ifdef Q_OS_MACOS
#include <sys/types.h>
#include <sys/sysctl.h>
#include <QOffscreenSurface>
#include "macos_context_type_helper.h"
#endif
#endif
#include <QThread>

#if QT_CONFIG(opengl)
QT_BEGIN_NAMESPACE
Q_GUI_EXPORT void qt_gl_set_global_share_context(QOpenGLContext *context);
Q_GUI_EXPORT QOpenGLContext *qt_gl_global_share_context();
QT_END_NAMESPACE
#endif

#if QT_CONFIG(opengl)
#ifdef Q_OS_MACOS
static bool needsOfflineRendererWorkaround()
{
    size_t hwmodelsize = 0;

    if (sysctlbyname("hw.model", nullptr, &hwmodelsize, nullptr, 0) == -1)
        return false;

    char hwmodel[hwmodelsize];
    if (sysctlbyname("hw.model", &hwmodel, &hwmodelsize, nullptr, 0) == -1)
        return false;

    return QString::fromLatin1(hwmodel) == QLatin1String("MacPro6,1");
}
#endif
#endif

namespace QtWebEngineCore {
#if QT_CONFIG(opengl)
static QOpenGLContext *shareContext;

static void deleteShareContext()
{
    if (qt_gl_global_share_context() == shareContext)
        qt_gl_set_global_share_context(nullptr);
    delete shareContext;
    shareContext = 0;
}

#endif
// ### Qt 6: unify this logic and Qt::AA_ShareOpenGLContexts.
// QtWebEngine::initialize was introduced first and meant to be called
// after the QGuiApplication creation, when AA_ShareOpenGLContexts fills
// the same need but the flag has to be set earlier.

Q_WEBENGINECORE_PRIVATE_EXPORT void initialize()
{
#if QT_CONFIG(opengl)
#ifdef Q_OS_WIN32
    qputenv("QT_D3DCREATE_MULTITHREADED", "1");
#endif
#ifdef Q_OS_MACOS
    if (needsOfflineRendererWorkaround())
        qputenv("QT_MAC_PRO_WEBENGINE_WORKAROUND", "1");
#endif
    // No need to override the shared context if QApplication already set one (e.g with Qt::AA_ShareOpenGLContexts).
    if (!qt_gl_global_share_context()) {

        QCoreApplication *app = QCoreApplication::instance();
        if (!app) {
            qFatal("QtWebEngine::initialize() but no core application instance.");
            return;
        }

        // Bail out silently if the user did not construct a QGuiApplication.
        if (!qobject_cast<QGuiApplication *>(app))
            return;

        if (app->thread() != QThread::currentThread()) {
            qFatal("QtWebEngine::initialize() must be called from the Qt gui thread.");
            return;
        }

        if (shareContext)
            return;

        shareContext = new QOpenGLContext;
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();

#if defined(Q_OS_MACOS)
        if (format == QSurfaceFormat()) {
            QOpenGLContext testContext;

            // Chromium turns off OpenGL for CoreProfiles with versions < 4.1
            // The newest Mac that only supports 3.3 was released in Mid 2011,
            // so it should be safe to request 4.1, but we still double check it
            // works in order not to set an invalid default surface format.
            format.setVersion(4, 1);
            format.setProfile(QSurfaceFormat::CoreProfile);

            testContext.setFormat(format);
            if (testContext.create()) {
                QOffscreenSurface surface;
                surface.setFormat(format);
                surface.create();

                if (testContext.makeCurrent(&surface)) {
                    // The Cocoa QPA integration allows sharing between OpenGL 3.2 and 4.1 contexts,
                    // which means even though we requested a 4.1 context, if we only get a 3.2
                    // context, it will still work an Chromium will not black list it.
                    if (testContext.format().version() >= qMakePair(3, 2)
                        && testContext.format().profile() == QSurfaceFormat::CoreProfile
                        && !isCurrentContextSoftware()) {
                        QSurfaceFormat::setDefaultFormat(format);
                    } else {
                        qWarning("The available OpenGL surface format was either not version 3.2 "
                                 "or higher or not a Core Profile.\n"
                                 "Chromium on macOS will fall back to software rendering in this "
                                 "case.\n"
                                 "Hardware acceleration and features such as WebGL will not be "
                                 "available.");
                        format = QSurfaceFormat::defaultFormat();
                    }
                    testContext.doneCurrent();
                }
                surface.destroy();
            }
        } else {
            // The user explicitly requested a specific surface format that does not fit Chromium's
            // requirements. Warn them about this.
            if (format.version() < qMakePair(3, 2)
                || format.profile() != QSurfaceFormat::CoreProfile) {
                qWarning("An OpenGL surfcace format was requested that is either not version 3.2 "
                         "or higher or a not Core Profile.\n"
                         "Chromium on macOS will fall back to software rendering in this case.\n"
                         "Hardware acceleration and features such as WebGL will not be available.");
            }
        }
#endif

        shareContext->setFormat(format);
        shareContext->create();
        qAddPostRoutine(deleteShareContext);
        qt_gl_set_global_share_context(shareContext);

        // Classes like QOpenGLWidget check for the attribute
        app->setAttribute(Qt::AA_ShareOpenGLContexts);
    }

#if defined(Q_OS_MACOS)
    // Check that the default QSurfaceFormat OpenGL profile is compatible with the global OpenGL
    // shared context profile, otherwise this could lead to a nasty crash.
    QSurfaceFormat sharedFormat = qt_gl_global_share_context()->format();
    QSurfaceFormat defaultFormat = QSurfaceFormat::defaultFormat();

    if (defaultFormat.profile() != sharedFormat.profile()
        && defaultFormat.profile() == QSurfaceFormat::CoreProfile
        && defaultFormat.version() >= qMakePair(3, 2)) {
        qFatal("QWebEngine: Default QSurfaceFormat OpenGL profile is not compatible with the "
               "global shared context OpenGL profile. Please make sure you set a compatible "
               "QSurfaceFormat before the QtGui application instance is created.");
    }
#endif

#endif // QT_CONFIG(opengl)
}
} // namespace QtWebEngineCore
