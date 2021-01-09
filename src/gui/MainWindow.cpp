/*
 *  Copyright (C) 2010 Felix Geyer <debfx@fobos.de>
 *  Copyright (C) 2020 KeePassXC Team <team@keepassxc.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "MainWindow.h"
#include "ui_MainWindow.h"

#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileInfo>
#include <QMimeData>
#include <QShortcut>
#include <QTimer>
#include <QWindow>

#include "config-keepassx.h"

#include "autotype/AutoType.h"
#include "core/Config.h"
#include "core/InactivityTimer.h"
#include "core/Metadata.h"
#include "core/Resources.h"
#include "core/Tools.h"
#include "gui/AboutDialog.h"
#include "gui/DatabaseWidget.h"
#include "gui/Icons.h"
#include "gui/MessageBox.h"
#include "gui/SearchWidget.h"
#include "gui/osutils/OSUtils.h"
#include "keys/CompositeKey.h"
#include "keys/FileKey.h"
#include "keys/PasswordKey.h"

#ifdef Q_OS_MACOS
#include "gui/osutils/macutils/MacUtils.h"
#ifdef WITH_XC_TOUCHID
#include "touchid/TouchID.h"
#endif
#endif

#ifdef WITH_XC_UPDATECHECK
#include "gui/UpdateCheckDialog.h"
#include "updatecheck/UpdateChecker.h"
#endif

#ifdef WITH_XC_SSHAGENT
#include "sshagent/AgentSettingsPage.h"
#include "sshagent/SSHAgent.h"
#endif
#ifdef WITH_XC_KEESHARE
#include "keeshare/KeeShare.h"
#include "keeshare/SettingsPageKeeShare.h"
#endif

#ifdef WITH_XC_FDOSECRETS
#include "fdosecrets/FdoSecretsPlugin.h"
#endif

#ifdef WITH_XC_YUBIKEY
#include "keys/drivers/YubiKey.h"
#endif

#ifdef WITH_XC_BROWSER
#include "browser/BrowserService.h"
#include "browser/BrowserSettingsPage.h"
#endif

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS) && !defined(QT_NO_DBUS)
#include "mainwindowadaptor.h"
#include <QList>
#include <QtDBus/QtDBus>
#endif

const QString MainWindow::BaseWindowTitle = "KeePassXC";

MainWindow* g_MainWindow = nullptr;
MainWindow* getMainWindow()
{
    return g_MainWindow;
}

MainWindow::MainWindow()
    : m_ui(new Ui::MainWindow())
{
    g_MainWindow = this;

    m_ui->setupUi(this);

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS) && !defined(QT_NO_DBUS)
    new MainWindowAdaptor(this);
    QDBusConnection dbus = QDBusConnection::sessionBus();
    dbus.registerObject("/keepassxc", this);
    dbus.registerService("org.keepassxc.KeePassXC.MainWindow");
#endif

    setAcceptDrops(true);

    if (config()->get(Config::GUI_CompactMode).toBool()) {
        m_ui->toolBar->setIconSize({20, 20});
    }

    // Setup the search widget in the toolbar
    m_searchWidget = new SearchWidget();
    m_searchWidget->connectSignals(m_actionMultiplexer);
    m_searchWidgetAction = m_ui->toolBar->addWidget(m_searchWidget);
    m_searchWidgetAction->setEnabled(false);

    m_countDefaultAttributes = m_ui->menuEntryCopyAttribute->actions().size();

    m_entryContextMenu = new QMenu(this);
    m_entryContextMenu->addAction(m_ui->actionEntryCopyUsername);
    m_entryContextMenu->addAction(m_ui->actionEntryCopyPassword);
    m_entryContextMenu->addAction(m_ui->menuEntryCopyAttribute->menuAction());
    m_entryContextMenu->addAction(m_ui->menuEntryTotp->menuAction());
    m_entryContextMenu->addSeparator();
    m_entryContextMenu->addAction(m_ui->actionEntryAutoType);
    m_entryContextMenu->addAction(m_ui->menuEntryAutoTypeWithSequence->menuAction());
    m_entryContextMenu->addSeparator();
    m_entryContextMenu->addAction(m_ui->actionEntryEdit);
    m_entryContextMenu->addAction(m_ui->actionEntryClone);
    m_entryContextMenu->addAction(m_ui->actionEntryDelete);
    m_entryContextMenu->addAction(m_ui->actionEntryNew);
    m_entryContextMenu->addSeparator();
    m_entryContextMenu->addAction(m_ui->actionEntryMoveUp);
    m_entryContextMenu->addAction(m_ui->actionEntryMoveDown);
    m_entryContextMenu->addSeparator();
    m_entryContextMenu->addAction(m_ui->actionEntryOpenUrl);
    m_entryContextMenu->addAction(m_ui->actionEntryDownloadIcon);

    m_entryNewContextMenu = new QMenu(this);
    m_entryNewContextMenu->addAction(m_ui->actionEntryNew);

    restoreGeometry(config()->get(Config::GUI_MainWindowGeometry).toByteArray());
    restoreState(config()->get(Config::GUI_MainWindowState).toByteArray());
#ifdef WITH_XC_BROWSER
    m_ui->settingsWidget->addSettingsPage(new BrowserSettingsPage());
    connect(m_ui->tabWidget, &DatabaseTabWidget::databaseLocked, browserService(), &BrowserService::databaseLocked);
    connect(m_ui->tabWidget, &DatabaseTabWidget::databaseUnlocked, browserService(), &BrowserService::databaseUnlocked);
    connect(m_ui->tabWidget,
            &DatabaseTabWidget::activateDatabaseChanged,
            browserService(),
            &BrowserService::activeDatabaseChanged);
    connect(
        browserService(), &BrowserService::requestUnlock, m_ui->tabWidget, &DatabaseTabWidget::performBrowserUnlock);
#endif

#ifdef WITH_XC_SSHAGENT
    connect(sshAgent(), SIGNAL(error(QString)), this, SLOT(showErrorMessage(QString)));
    connect(sshAgent(), SIGNAL(enabledChanged(bool)), this, SLOT(agentEnabled(bool)));
    m_ui->settingsWidget->addSettingsPage(new AgentSettingsPage());

    m_entryContextMenu->addSeparator();
    m_entryContextMenu->addAction(m_ui->actionEntryAddToAgent);
    m_entryContextMenu->addAction(m_ui->actionEntryRemoveFromAgent);

    m_ui->actionEntryAddToAgent->setIcon(icons()->icon("utilities-terminal"));
    m_ui->actionEntryRemoveFromAgent->setIcon(icons()->icon("utilities-terminal"));
#endif

    m_ui->actionEntryAddToAgent->setVisible(false);
    m_ui->actionEntryRemoveFromAgent->setVisible(false);

    initViewMenu();

#if defined(WITH_XC_KEESHARE)
    KeeShare::init(this);
    m_ui->settingsWidget->addSettingsPage(new SettingsPageKeeShare(m_ui->tabWidget));
    connect(KeeShare::instance(),
            SIGNAL(sharingMessage(QString, MessageWidget::MessageType)),
            SLOT(displayGlobalMessage(QString, MessageWidget::MessageType)));
#endif

#ifdef WITH_XC_FDOSECRETS
    auto fdoSS = new FdoSecretsPlugin(m_ui->tabWidget);
    connect(fdoSS, &FdoSecretsPlugin::error, this, &MainWindow::showErrorMessage);
    connect(fdoSS, &FdoSecretsPlugin::requestSwitchToDatabases, this, &MainWindow::switchToDatabases);
    connect(fdoSS, &FdoSecretsPlugin::requestShowNotification, this, &MainWindow::displayDesktopNotification);
    fdoSS->updateServiceState();
    m_ui->settingsWidget->addSettingsPage(fdoSS);
#endif

#ifdef WITH_XC_YUBIKEY
    connect(YubiKey::instance(), SIGNAL(userInteractionRequest()), SLOT(showYubiKeyPopup()), Qt::QueuedConnection);
    connect(YubiKey::instance(), SIGNAL(challengeCompleted()), SLOT(hideYubiKeyPopup()), Qt::QueuedConnection);
#endif

    setWindowIcon(icons()->applicationIcon());
    m_ui->globalMessageWidget->hideMessage();
    connect(m_ui->globalMessageWidget, &MessageWidget::linkActivated, &MessageWidget::openHttpUrl);

    m_clearHistoryAction = new QAction(tr("Clear history"), m_ui->menuFile);
    m_lastDatabasesActions = new QActionGroup(m_ui->menuRecentDatabases);
    connect(m_clearHistoryAction, SIGNAL(triggered()), this, SLOT(clearLastDatabases()));
    connect(m_lastDatabasesActions, SIGNAL(triggered(QAction*)), this, SLOT(openRecentDatabase(QAction*)));
    connect(m_ui->menuRecentDatabases, SIGNAL(aboutToShow()), this, SLOT(updateLastDatabasesMenu()));

    m_copyAdditionalAttributeActions = new QActionGroup(m_ui->menuEntryCopyAttribute);
    m_actionMultiplexer.connect(
        m_copyAdditionalAttributeActions, SIGNAL(triggered(QAction*)), SLOT(copyAttribute(QAction*)));
    connect(m_ui->menuEntryCopyAttribute, SIGNAL(aboutToShow()), this, SLOT(updateCopyAttributesMenu()));

    Qt::Key globalAutoTypeKey = static_cast<Qt::Key>(config()->get(Config::GlobalAutoTypeKey).toInt());
    Qt::KeyboardModifiers globalAutoTypeModifiers =
        static_cast<Qt::KeyboardModifiers>(config()->get(Config::GlobalAutoTypeModifiers).toInt());
    if (globalAutoTypeKey > 0 && globalAutoTypeModifiers > 0) {
        autoType()->registerGlobalShortcut(globalAutoTypeKey, globalAutoTypeModifiers);
    }

    m_ui->toolbarSeparator->setVisible(false);
    m_showToolbarSeparator = config()->get(Config::GUI_ApplicationTheme).toString() != "classic";

    bool isAutoTypeAvailable = autoType()->isAvailable();
    m_ui->actionEntryAutoType->setVisible(isAutoTypeAvailable);
    m_ui->actionEntryAutoTypeUsername->setVisible(isAutoTypeAvailable);
    m_ui->actionEntryAutoTypeUsernameEnter->setVisible(isAutoTypeAvailable);
    m_ui->actionEntryAutoTypePassword->setVisible(isAutoTypeAvailable);
    m_ui->actionEntryAutoTypePasswordEnter->setVisible(isAutoTypeAvailable);

    m_inactivityTimer = new InactivityTimer(this);
    connect(m_inactivityTimer, SIGNAL(inactivityDetected()), this, SLOT(lockDatabasesAfterInactivity()));
#ifdef WITH_XC_TOUCHID
    m_touchIDinactivityTimer = new InactivityTimer(this);
    connect(m_touchIDinactivityTimer, SIGNAL(inactivityDetected()), this, SLOT(forgetTouchIDAfterInactivity()));
#endif
    applySettingsChanges();

    m_ui->actionDatabaseNew->setShortcut(Qt::CTRL + Qt::SHIFT + Qt::Key_N);
    setShortcut(m_ui->actionDatabaseOpen, QKeySequence::Open, Qt::CTRL + Qt::Key_O);
    setShortcut(m_ui->actionDatabaseSave, QKeySequence::Save, Qt::CTRL + Qt::Key_S);
    setShortcut(m_ui->actionDatabaseSaveAs, QKeySequence::SaveAs, Qt::CTRL + Qt::SHIFT + Qt::Key_S);
    setShortcut(m_ui->actionDatabaseClose, QKeySequence::Close, Qt::CTRL + Qt::Key_W);
    m_ui->actionLockDatabases->setShortcut(Qt::CTRL + Qt::Key_L);
    setShortcut(m_ui->actionQuit, QKeySequence::Quit, Qt::CTRL + Qt::Key_Q);
    setShortcut(m_ui->actionEntryNew, QKeySequence::New, Qt::CTRL + Qt::Key_N);
    m_ui->actionEntryEdit->setShortcut(Qt::CTRL + Qt::Key_E);
    m_ui->actionEntryDelete->setShortcut(Qt::CTRL + Qt::Key_D);
    m_ui->actionEntryDelete->setShortcut(Qt::Key_Delete);
    m_ui->actionEntryClone->setShortcut(Qt::CTRL + Qt::Key_K);
    m_ui->actionEntryTotp->setShortcut(Qt::CTRL + Qt::SHIFT + Qt::Key_T);
    m_ui->actionEntryDownloadIcon->setShortcut(Qt::CTRL + Qt::SHIFT + Qt::Key_D);
    m_ui->actionEntryCopyTotp->setShortcut(Qt::CTRL + Qt::Key_T);
    m_ui->actionEntryMoveUp->setShortcut(Qt::CTRL + Qt::ALT + Qt::Key_Up);
    m_ui->actionEntryMoveDown->setShortcut(Qt::CTRL + Qt::ALT + Qt::Key_Down);
    m_ui->actionEntryCopyUsername->setShortcut(Qt::CTRL + Qt::Key_B);
    m_ui->actionEntryCopyPassword->setShortcut(Qt::CTRL + Qt::Key_C);
    m_ui->actionEntryAutoType->setShortcut(Qt::CTRL + Qt::SHIFT + Qt::Key_V);
    m_ui->actionEntryOpenUrl->setShortcut(Qt::CTRL + Qt::SHIFT + Qt::Key_U);
    m_ui->actionEntryCopyURL->setShortcut(Qt::CTRL + Qt::Key_U);

    // Prevent conflicts with global Mac shortcuts (force Control on all platforms)
#ifdef Q_OS_MAC
    auto modifier = Qt::META;
#else
    auto modifier = Qt::CTRL;
#endif
    m_ui->actionEntryAddToAgent->setShortcut(modifier + Qt::Key_H);
    m_ui->actionEntryRemoveFromAgent->setShortcut(modifier + Qt::SHIFT + Qt::Key_H);

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    // Qt 5.10 introduced a new "feature" to hide shortcuts in context menus
    // Unfortunately, Qt::AA_DontShowShortcutsInContextMenus is broken, have to manually enable them
    m_ui->actionEntryNew->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryEdit->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryDelete->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryClone->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryTotp->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryDownloadIcon->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryCopyTotp->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryMoveUp->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryMoveDown->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryCopyUsername->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryCopyPassword->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryAutoType->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryOpenUrl->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryCopyURL->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryAddToAgent->setShortcutVisibleInContextMenu(true);
    m_ui->actionEntryRemoveFromAgent->setShortcutVisibleInContextMenu(true);
#endif

    connect(m_ui->menuEntries, SIGNAL(aboutToShow()), SLOT(obtainContextFocusLock()));
    connect(m_ui->menuEntries, SIGNAL(aboutToHide()), SLOT(releaseContextFocusLock()));
    connect(m_entryContextMenu, SIGNAL(aboutToShow()), SLOT(obtainContextFocusLock()));
    connect(m_entryContextMenu, SIGNAL(aboutToHide()), SLOT(releaseContextFocusLock()));
    connect(m_entryNewContextMenu, SIGNAL(aboutToShow()), SLOT(obtainContextFocusLock()));
    connect(m_entryNewContextMenu, SIGNAL(aboutToHide()), SLOT(releaseContextFocusLock()));
    connect(m_ui->menuGroups, SIGNAL(aboutToShow()), SLOT(obtainContextFocusLock()));
    connect(m_ui->menuGroups, SIGNAL(aboutToHide()), SLOT(releaseContextFocusLock()));

    // Control window state
    new QShortcut(Qt::CTRL + Qt::Key_M, this, SLOT(minimizeOrHide()));
    new QShortcut(Qt::CTRL + Qt::SHIFT + Qt::Key_M, this, SLOT(hideWindow()));
    // Control database tabs
    // Ctrl+Tab is broken on Mac, so use Alt (i.e. the Option key) - https://bugreports.qt.io/browse/QTBUG-8596
    auto dbTabModifier2 = Qt::CTRL;
#ifdef Q_OS_MACOS
    dbTabModifier2 = Qt::ALT;
#endif
    new QShortcut(dbTabModifier2 + Qt::Key_Tab, this, SLOT(selectNextDatabaseTab()));
    new QShortcut(Qt::CTRL + Qt::Key_PageDown, this, SLOT(selectNextDatabaseTab()));
    new QShortcut(dbTabModifier2 + Qt::SHIFT + Qt::Key_Tab, this, SLOT(selectPreviousDatabaseTab()));
    new QShortcut(Qt::CTRL + Qt::Key_PageUp, this, SLOT(selectPreviousDatabaseTab()));

    // Tab selection by number, Windows uses Ctrl, macOS uses Command,
    // and Linux uses Alt to emulate a browser-like experience
    auto dbTabModifier = Qt::CTRL;
#ifdef Q_OS_LINUX
    dbTabModifier = Qt::ALT;
#endif
    auto shortcut = new QShortcut(dbTabModifier + Qt::Key_1, this);
    connect(shortcut, &QShortcut::activated, [this]() { selectDatabaseTab(0); });
    shortcut = new QShortcut(dbTabModifier + Qt::Key_2, this);
    connect(shortcut, &QShortcut::activated, [this]() { selectDatabaseTab(1); });
    shortcut = new QShortcut(dbTabModifier + Qt::Key_3, this);
    connect(shortcut, &QShortcut::activated, [this]() { selectDatabaseTab(2); });
    shortcut = new QShortcut(dbTabModifier + Qt::Key_4, this);
    connect(shortcut, &QShortcut::activated, [this]() { selectDatabaseTab(3); });
    shortcut = new QShortcut(dbTabModifier + Qt::Key_5, this);
    connect(shortcut, &QShortcut::activated, [this]() { selectDatabaseTab(4); });
    shortcut = new QShortcut(dbTabModifier + Qt::Key_6, this);
    connect(shortcut, &QShortcut::activated, [this]() { selectDatabaseTab(5); });
    shortcut = new QShortcut(dbTabModifier + Qt::Key_7, this);
    connect(shortcut, &QShortcut::activated, [this]() { selectDatabaseTab(6); });
    shortcut = new QShortcut(dbTabModifier + Qt::Key_8, this);
    connect(shortcut, &QShortcut::activated, [this]() { selectDatabaseTab(7); });
    shortcut = new QShortcut(dbTabModifier + Qt::Key_9, this);
    connect(shortcut, &QShortcut::activated, [this]() { selectDatabaseTab(m_ui->tabWidget->count() - 1); });

    m_ui->actionDatabaseNew->setIcon(icons()->icon("document-new"));
    m_ui->actionDatabaseOpen->setIcon(icons()->icon("document-open"));
    m_ui->menuRecentDatabases->setIcon(icons()->icon("document-open-recent"));
    m_ui->actionDatabaseSave->setIcon(icons()->icon("document-save"));
    m_ui->actionDatabaseSaveAs->setIcon(icons()->icon("document-save-as"));
    m_ui->actionDatabaseSaveBackup->setIcon(icons()->icon("document-save-copy"));
    m_ui->actionDatabaseClose->setIcon(icons()->icon("document-close"));
    m_ui->actionReports->setIcon(icons()->icon("reports"));
    m_ui->actionDatabaseSettings->setIcon(icons()->icon("document-edit"));
    m_ui->actionDatabaseSecurity->setIcon(icons()->icon("database-change-key"));
    m_ui->actionLockDatabases->setIcon(icons()->icon("database-lock"));
    m_ui->actionQuit->setIcon(icons()->icon("application-exit"));
    m_ui->actionDatabaseMerge->setIcon(icons()->icon("database-merge"));
    m_ui->menuImport->setIcon(icons()->icon("document-import"));
    m_ui->menuExport->setIcon(icons()->icon("document-export"));

    m_ui->actionEntryNew->setIcon(icons()->icon("entry-new"));
    m_ui->actionEntryClone->setIcon(icons()->icon("entry-clone"));
    m_ui->actionEntryEdit->setIcon(icons()->icon("entry-edit"));
    m_ui->actionEntryDelete->setIcon(icons()->icon("entry-delete"));
    m_ui->actionEntryAutoType->setIcon(icons()->icon("auto-type"));
    m_ui->menuEntryAutoTypeWithSequence->setIcon(icons()->icon("auto-type"));
    m_ui->actionEntryAutoTypeUsername->setIcon(icons()->icon("auto-type"));
    m_ui->actionEntryAutoTypeUsernameEnter->setIcon(icons()->icon("auto-type"));
    m_ui->actionEntryAutoTypePassword->setIcon(icons()->icon("auto-type"));
    m_ui->actionEntryAutoTypePasswordEnter->setIcon(icons()->icon("auto-type"));
    m_ui->actionEntryMoveUp->setIcon(icons()->icon("move-up"));
    m_ui->actionEntryMoveDown->setIcon(icons()->icon("move-down"));
    m_ui->actionEntryCopyUsername->setIcon(icons()->icon("username-copy"));
    m_ui->actionEntryCopyPassword->setIcon(icons()->icon("password-copy"));
    m_ui->actionEntryCopyURL->setIcon(icons()->icon("url-copy"));
    m_ui->actionEntryDownloadIcon->setIcon(icons()->icon("favicon-download"));
    m_ui->actionGroupSortAsc->setIcon(icons()->icon("sort-alphabetical-ascending"));
    m_ui->actionGroupSortDesc->setIcon(icons()->icon("sort-alphabetical-descending"));

    m_ui->actionGroupNew->setIcon(icons()->icon("group-new"));
    m_ui->actionGroupEdit->setIcon(icons()->icon("group-edit"));
    m_ui->actionGroupDelete->setIcon(icons()->icon("group-delete"));
    m_ui->actionGroupEmptyRecycleBin->setIcon(icons()->icon("group-empty-trash"));
    m_ui->actionEntryOpenUrl->setIcon(icons()->icon("web"));
    m_ui->actionGroupDownloadFavicons->setIcon(icons()->icon("favicon-download"));

    m_ui->actionSettings->setIcon(icons()->icon("configure"));
    m_ui->actionPasswordGenerator->setIcon(icons()->icon("password-generator"));

    m_ui->actionAbout->setIcon(icons()->icon("help-about"));
    m_ui->actionDonate->setIcon(icons()->icon("donate"));
    m_ui->actionBugReport->setIcon(icons()->icon("bugreport"));
    m_ui->actionGettingStarted->setIcon(icons()->icon("getting-started"));
    m_ui->actionUserGuide->setIcon(icons()->icon("user-guide"));
    m_ui->actionOnlineHelp->setIcon(icons()->icon("system-help"));
    m_ui->actionKeyboardShortcuts->setIcon(icons()->icon("keyboard-shortcuts"));
    m_ui->actionCheckForUpdates->setIcon(icons()->icon("system-software-update"));

    m_actionMultiplexer.connect(
        SIGNAL(currentModeChanged(DatabaseWidget::Mode)), this, SLOT(setMenuActionState(DatabaseWidget::Mode)));
    m_actionMultiplexer.connect(SIGNAL(groupChanged()), this, SLOT(setMenuActionState()));
    m_actionMultiplexer.connect(SIGNAL(entrySelectionChanged()), this, SLOT(setMenuActionState()));
    m_actionMultiplexer.connect(SIGNAL(groupContextMenuRequested(QPoint)), this, SLOT(showGroupContextMenu(QPoint)));
    m_actionMultiplexer.connect(SIGNAL(entryContextMenuRequested(QPoint)), this, SLOT(showEntryContextMenu(QPoint)));

    // Notify search when the active database changes or gets locked
    connect(m_ui->tabWidget,
            SIGNAL(activateDatabaseChanged(DatabaseWidget*)),
            m_searchWidget,
            SLOT(databaseChanged(DatabaseWidget*)));
    connect(m_ui->tabWidget, SIGNAL(databaseLocked(DatabaseWidget*)), m_searchWidget, SLOT(databaseChanged()));

    connect(m_ui->tabWidget, SIGNAL(tabNameChanged()), SLOT(updateWindowTitle()));
    connect(m_ui->tabWidget, SIGNAL(currentChanged(int)), SLOT(updateWindowTitle()));
    connect(m_ui->tabWidget, SIGNAL(currentChanged(int)), SLOT(databaseTabChanged(int)));
    connect(m_ui->tabWidget, SIGNAL(currentChanged(int)), SLOT(setMenuActionState()));
    connect(m_ui->tabWidget, SIGNAL(currentChanged(int)), SLOT(updateTrayIcon()));
    connect(m_ui->tabWidget, SIGNAL(databaseLocked(DatabaseWidget*)), SLOT(databaseStatusChanged(DatabaseWidget*)));
    connect(m_ui->tabWidget, SIGNAL(databaseUnlocked(DatabaseWidget*)), SLOT(databaseStatusChanged(DatabaseWidget*)));
    connect(m_ui->tabWidget, SIGNAL(tabVisibilityChanged(bool)), SLOT(updateToolbarSeparatorVisibility()));
    connect(m_ui->stackedWidget, SIGNAL(currentChanged(int)), SLOT(setMenuActionState()));
    connect(m_ui->stackedWidget, SIGNAL(currentChanged(int)), SLOT(updateWindowTitle()));
    connect(m_ui->stackedWidget, SIGNAL(currentChanged(int)), SLOT(updateToolbarSeparatorVisibility()));
    connect(m_ui->settingsWidget, SIGNAL(accepted()), SLOT(applySettingsChanges()));
    connect(m_ui->settingsWidget, SIGNAL(settingsReset()), SLOT(applySettingsChanges()));
    connect(m_ui->settingsWidget, SIGNAL(accepted()), SLOT(switchToDatabases()));
    connect(m_ui->settingsWidget, SIGNAL(rejected()), SLOT(switchToDatabases()));

    connect(m_ui->actionDatabaseNew, SIGNAL(triggered()), m_ui->tabWidget, SLOT(newDatabase()));
    connect(m_ui->actionDatabaseOpen, SIGNAL(triggered()), m_ui->tabWidget, SLOT(openDatabase()));
    connect(m_ui->actionDatabaseSave, SIGNAL(triggered()), m_ui->tabWidget, SLOT(saveDatabase()));
    connect(m_ui->actionDatabaseSaveAs, SIGNAL(triggered()), m_ui->tabWidget, SLOT(saveDatabaseAs()));
    connect(m_ui->actionDatabaseSaveBackup, SIGNAL(triggered()), m_ui->tabWidget, SLOT(saveDatabaseBackup()));
    connect(m_ui->actionDatabaseClose, SIGNAL(triggered()), m_ui->tabWidget, SLOT(closeCurrentDatabaseTab()));
    connect(m_ui->actionDatabaseMerge, SIGNAL(triggered()), m_ui->tabWidget, SLOT(mergeDatabase()));
    connect(m_ui->actionDatabaseSecurity, SIGNAL(triggered()), m_ui->tabWidget, SLOT(showDatabaseSecurity()));
    connect(m_ui->actionReports, SIGNAL(triggered()), m_ui->tabWidget, SLOT(showDatabaseReports()));
    connect(m_ui->actionDatabaseSettings, SIGNAL(triggered()), m_ui->tabWidget, SLOT(showDatabaseSettings()));
    connect(m_ui->actionImportCsv, SIGNAL(triggered()), m_ui->tabWidget, SLOT(importCsv()));
    connect(m_ui->actionImportKeePass1, SIGNAL(triggered()), m_ui->tabWidget, SLOT(importKeePass1Database()));
    connect(m_ui->actionImportOpVault, SIGNAL(triggered()), m_ui->tabWidget, SLOT(importOpVaultDatabase()));
    connect(m_ui->actionExportCsv, SIGNAL(triggered()), m_ui->tabWidget, SLOT(exportToCsv()));
    connect(m_ui->actionExportHtml, SIGNAL(triggered()), m_ui->tabWidget, SLOT(exportToHtml()));
    connect(m_ui->actionLockDatabases, SIGNAL(triggered()), m_ui->tabWidget, SLOT(lockDatabases()));
    connect(m_ui->actionQuit, SIGNAL(triggered()), SLOT(appExit()));

    m_actionMultiplexer.connect(m_ui->actionEntryNew, SIGNAL(triggered()), SLOT(createEntry()));
    m_actionMultiplexer.connect(m_ui->actionEntryClone, SIGNAL(triggered()), SLOT(cloneEntry()));
    m_actionMultiplexer.connect(m_ui->actionEntryEdit, SIGNAL(triggered()), SLOT(switchToEntryEdit()));
    m_actionMultiplexer.connect(m_ui->actionEntryDelete, SIGNAL(triggered()), SLOT(deleteSelectedEntries()));

    m_actionMultiplexer.connect(m_ui->actionEntryTotp, SIGNAL(triggered()), SLOT(showTotp()));
    m_actionMultiplexer.connect(m_ui->actionEntrySetupTotp, SIGNAL(triggered()), SLOT(setupTotp()));

    m_actionMultiplexer.connect(m_ui->actionEntryCopyTotp, SIGNAL(triggered()), SLOT(copyTotp()));
    m_actionMultiplexer.connect(m_ui->actionEntryTotpQRCode, SIGNAL(triggered()), SLOT(showTotpKeyQrCode()));
    m_actionMultiplexer.connect(m_ui->actionEntryCopyTitle, SIGNAL(triggered()), SLOT(copyTitle()));
    m_actionMultiplexer.connect(m_ui->actionEntryMoveUp, SIGNAL(triggered()), SLOT(moveEntryUp()));
    m_actionMultiplexer.connect(m_ui->actionEntryMoveDown, SIGNAL(triggered()), SLOT(moveEntryDown()));
    m_actionMultiplexer.connect(m_ui->actionEntryCopyUsername, SIGNAL(triggered()), SLOT(copyUsername()));
    m_actionMultiplexer.connect(m_ui->actionEntryCopyPassword, SIGNAL(triggered()), SLOT(copyPassword()));
    m_actionMultiplexer.connect(m_ui->actionEntryCopyURL, SIGNAL(triggered()), SLOT(copyURL()));
    m_actionMultiplexer.connect(m_ui->actionEntryCopyNotes, SIGNAL(triggered()), SLOT(copyNotes()));
    m_actionMultiplexer.connect(m_ui->actionEntryAutoType, SIGNAL(triggered()), SLOT(performAutoType()));
    m_actionMultiplexer.connect(
        m_ui->actionEntryAutoTypeUsername, SIGNAL(triggered()), SLOT(performAutoTypeUsername()));
    m_actionMultiplexer.connect(
        m_ui->actionEntryAutoTypeUsernameEnter, SIGNAL(triggered()), SLOT(performAutoTypeUsernameEnter()));
    m_actionMultiplexer.connect(
        m_ui->actionEntryAutoTypePassword, SIGNAL(triggered()), SLOT(performAutoTypePassword()));
    m_actionMultiplexer.connect(
        m_ui->actionEntryAutoTypePasswordEnter, SIGNAL(triggered()), SLOT(performAutoTypePasswordEnter()));
    m_actionMultiplexer.connect(m_ui->actionEntryOpenUrl, SIGNAL(triggered()), SLOT(openUrl()));
    m_actionMultiplexer.connect(m_ui->actionEntryDownloadIcon, SIGNAL(triggered()), SLOT(downloadSelectedFavicons()));
#ifdef WITH_XC_SSHAGENT
    m_actionMultiplexer.connect(m_ui->actionEntryAddToAgent, SIGNAL(triggered()), SLOT(addToAgent()));
    m_actionMultiplexer.connect(m_ui->actionEntryRemoveFromAgent, SIGNAL(triggered()), SLOT(removeFromAgent()));
#endif

    m_actionMultiplexer.connect(m_ui->actionGroupNew, SIGNAL(triggered()), SLOT(createGroup()));
    m_actionMultiplexer.connect(m_ui->actionGroupEdit, SIGNAL(triggered()), SLOT(switchToGroupEdit()));
    m_actionMultiplexer.connect(m_ui->actionGroupDelete, SIGNAL(triggered()), SLOT(deleteGroup()));
    m_actionMultiplexer.connect(m_ui->actionGroupEmptyRecycleBin, SIGNAL(triggered()), SLOT(emptyRecycleBin()));
    m_actionMultiplexer.connect(m_ui->actionGroupSortAsc, SIGNAL(triggered()), SLOT(sortGroupsAsc()));
    m_actionMultiplexer.connect(m_ui->actionGroupSortDesc, SIGNAL(triggered()), SLOT(sortGroupsDesc()));
    m_actionMultiplexer.connect(m_ui->actionGroupDownloadFavicons, SIGNAL(triggered()), SLOT(downloadAllFavicons()));

    connect(m_ui->actionSettings, SIGNAL(toggled(bool)), SLOT(switchToSettings(bool)));
    connect(m_ui->actionPasswordGenerator, SIGNAL(toggled(bool)), SLOT(togglePasswordGenerator(bool)));
    connect(m_ui->passwordGeneratorWidget, &PasswordGeneratorWidget::closed, this, [this] {
        togglePasswordGenerator(false);
    });
    m_ui->passwordGeneratorWidget->setStandaloneMode(true);

    connect(m_ui->welcomeWidget, SIGNAL(newDatabase()), SLOT(switchToNewDatabase()));
    connect(m_ui->welcomeWidget, SIGNAL(openDatabase()), SLOT(switchToOpenDatabase()));
    connect(m_ui->welcomeWidget, SIGNAL(openDatabaseFile(QString)), SLOT(switchToDatabaseFile(QString)));
    connect(m_ui->welcomeWidget, SIGNAL(importKeePass1Database()), SLOT(switchToKeePass1Database()));
    connect(m_ui->welcomeWidget, SIGNAL(importOpVaultDatabase()), SLOT(switchToOpVaultDatabase()));
    connect(m_ui->welcomeWidget, SIGNAL(importCsv()), SLOT(switchToCsvImport()));

    connect(m_ui->actionAbout, SIGNAL(triggered()), SLOT(showAboutDialog()));
    connect(m_ui->actionDonate, SIGNAL(triggered()), SLOT(openDonateUrl()));
    connect(m_ui->actionBugReport, SIGNAL(triggered()), SLOT(openBugReportUrl()));
    connect(m_ui->actionGettingStarted, SIGNAL(triggered()), SLOT(openGettingStartedGuide()));
    connect(m_ui->actionUserGuide, SIGNAL(triggered()), SLOT(openUserGuide()));
    connect(m_ui->actionOnlineHelp, SIGNAL(triggered()), SLOT(openOnlineHelp()));
    connect(m_ui->actionKeyboardShortcuts, SIGNAL(triggered()), SLOT(openKeyboardShortcuts()));

    connect(osUtils, &OSUtilsBase::statusbarThemeChanged, this, &MainWindow::updateTrayIcon);

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    // Install event filter for empty-area drag
    auto* eventFilter = new MainWindowEventFilter(this);
    m_ui->menubar->installEventFilter(eventFilter);
    m_ui->toolBar->installEventFilter(eventFilter);
    m_ui->tabWidget->tabBar()->installEventFilter(eventFilter);
#endif

#ifdef Q_OS_MACOS
    setUnifiedTitleAndToolBarOnMac(true);
#endif

#ifdef WITH_XC_UPDATECHECK
    connect(m_ui->actionCheckForUpdates, SIGNAL(triggered()), SLOT(showUpdateCheckDialog()));
    connect(UpdateChecker::instance(),
            SIGNAL(updateCheckFinished(bool, QString, bool)),
            SLOT(hasUpdateAvailable(bool, QString, bool)));
    // Setup an update check every hour (checked only occur every 7 days)
    connect(&m_updateCheckTimer, &QTimer::timeout, this, &MainWindow::performUpdateCheck);
    m_updateCheckTimer.start(3.6e6);
    // Perform the startup update check after 500 ms
    QTimer::singleShot(500, this, SLOT(performUpdateCheck()));
#else
    m_ui->actionCheckForUpdates->setVisible(false);
#endif

#ifndef WITH_XC_NETWORKING
    m_ui->actionGroupDownloadFavicons->setVisible(false);
    m_ui->actionEntryDownloadIcon->setVisible(false);
#endif

    // clang-format off
    connect(m_ui->tabWidget, SIGNAL(messageGlobal(QString,MessageWidget::MessageType)),
        SLOT(displayGlobalMessage(QString,MessageWidget::MessageType)));
    // clang-format on

    connect(m_ui->tabWidget, SIGNAL(messageDismissGlobal()), this, SLOT(hideGlobalMessage()));

#ifndef Q_OS_HAIKU
    m_screenLockListener = new ScreenLockListener(this);
    connect(m_screenLockListener, SIGNAL(screenLocked()), SLOT(handleScreenLock()));
#endif

    // Tray Icon setup
    connect(Application::instance(), SIGNAL(focusWindowChanged(QWindow*)), SLOT(focusWindowChanged(QWindow*)));
    m_trayIconTriggerReason = QSystemTrayIcon::Unknown;
    m_trayIconTriggerTimer.setSingleShot(true);
    connect(&m_trayIconTriggerTimer, SIGNAL(timeout()), SLOT(processTrayIconTrigger()));

    if (config()->hasAccessError()) {
        m_ui->globalMessageWidget->showMessage(tr("Access error for config file %1").arg(config()->getFileName()),
                                               MessageWidget::Error);
    }

#if defined(KEEPASSXC_BUILD_TYPE_SNAPSHOT) || defined(KEEPASSXC_BUILD_TYPE_PRE_RELEASE)
    auto* hidePreRelWarn = new QAction(tr("Don't show again for this version"), m_ui->globalMessageWidget);
    m_ui->globalMessageWidget->addAction(hidePreRelWarn);
    auto hidePreRelWarnConn = QSharedPointer<QMetaObject::Connection>::create();
    *hidePreRelWarnConn = connect(m_ui->globalMessageWidget, &KMessageWidget::hideAnimationFinished, [=] {
        m_ui->globalMessageWidget->removeAction(hidePreRelWarn);
        disconnect(*hidePreRelWarnConn);
        hidePreRelWarn->deleteLater();
    });
    connect(hidePreRelWarn, &QAction::triggered, [=] {
        m_ui->globalMessageWidget->animatedHide();
        config()->set(Config::Messages_HidePreReleaseWarning, KEEPASSXC_VERSION);
    });
#endif
#if defined(KEEPASSXC_BUILD_TYPE_SNAPSHOT)
    if (config()->get(Config::Messages_HidePreReleaseWarning) != KEEPASSXC_VERSION) {
        m_ui->globalMessageWidget->showMessage(
            tr("WARNING: You are using an unstable build of KeePassXC!\n"
               "There is a high risk of corruption, maintain a backup of your databases.\n"
               "This version is not meant for production use."),
            MessageWidget::Warning,
            -1);
    }
#elif defined(KEEPASSXC_BUILD_TYPE_PRE_RELEASE)
    if (config()->get(Config::Messages_HidePreReleaseWarning) != KEEPASSXC_VERSION) {
        m_ui->globalMessageWidget->showMessage(
            tr("NOTE: You are using a pre-release version of KeePassXC!\n"
               "Expect some bugs and minor issues, this version is not meant for production use."),
            MessageWidget::Information,
            -1);
    }
#elif (QT_VERSION >= QT_VERSION_CHECK(5, 5, 0) && QT_VERSION < QT_VERSION_CHECK(5, 6, 0))
    if (!config()->get(Config::Messages_Qt55CompatibilityWarning).toBool()) {
        m_ui->globalMessageWidget->showMessage(
            tr("WARNING: Your Qt version may cause KeePassXC to crash with an On-Screen Keyboard!\n"
               "We recommend you use the AppImage available on our downloads page."),
            MessageWidget::Warning,
            -1);
        config()->set(Config::Messages_Qt55CompatibilityWarning, true);
    }
#endif

    connect(qApp, SIGNAL(anotherInstanceStarted()), this, SLOT(bringToFront()));
    connect(qApp, SIGNAL(applicationActivated()), this, SLOT(bringToFront()));
    connect(qApp, SIGNAL(openFile(QString)), this, SLOT(openDatabase(QString)));
    connect(qApp, SIGNAL(quitSignalReceived()), this, SLOT(appExit()), Qt::DirectConnection);

    restoreConfigState();
}

MainWindow::~MainWindow()
{
}

/**
 * Restore the main window's state after launch
 */
void MainWindow::restoreConfigState()
{
    // start minimized if configured
    if (config()->get(Config::GUI_MinimizeOnStartup).toBool()) {
        hideWindow();
    } else {
        bringToFront();
    }

    if (config()->get(Config::OpenPreviousDatabasesOnStartup).toBool()) {
        const QStringList fileNames = config()->get(Config::LastOpenedDatabases).toStringList();
        for (const QString& filename : fileNames) {
            if (!filename.isEmpty() && QFile::exists(filename)) {
                openDatabase(filename);
            }
        }
        auto lastActiveFile = config()->get(Config::LastActiveDatabase).toString();
        if (!lastActiveFile.isEmpty()) {
            openDatabase(lastActiveFile);
        }
    }
}

QList<DatabaseWidget*> MainWindow::getOpenDatabases()
{
    QList<DatabaseWidget*> dbWidgets;
    for (int i = 0; i < m_ui->tabWidget->count(); ++i) {
        dbWidgets << m_ui->tabWidget->databaseWidgetFromIndex(i);
    }
    return dbWidgets;
}

void MainWindow::showErrorMessage(const QString& message)
{
    m_ui->globalMessageWidget->showMessage(message, MessageWidget::Error);
}

void MainWindow::appExit()
{
    m_appExitCalled = true;
    close();
}

void MainWindow::updateLastDatabasesMenu()
{
    m_ui->menuRecentDatabases->clear();

    const QStringList lastDatabases = config()->get(Config::LastDatabases).toStringList();
    for (const QString& database : lastDatabases) {
        QAction* action = m_ui->menuRecentDatabases->addAction(database);
        action->setData(database);
        m_lastDatabasesActions->addAction(action);
    }
    m_ui->menuRecentDatabases->addSeparator();
    m_ui->menuRecentDatabases->addAction(m_clearHistoryAction);
}

void MainWindow::updateCopyAttributesMenu()
{
    DatabaseWidget* dbWidget = m_ui->tabWidget->currentDatabaseWidget();
    if (!dbWidget) {
        return;
    }

    if (dbWidget->numberOfSelectedEntries() != 1) {
        return;
    }

    QList<QAction*> actions = m_ui->menuEntryCopyAttribute->actions();
    for (int i = m_countDefaultAttributes; i < actions.size(); i++) {
        delete actions[i];
    }

    const QStringList customEntryAttributes = dbWidget->customEntryAttributes();
    for (const QString& key : customEntryAttributes) {
        QAction* action = m_ui->menuEntryCopyAttribute->addAction(key);
        action->setData(QVariant(key));
        m_copyAdditionalAttributeActions->addAction(action);
    }
}

void MainWindow::openRecentDatabase(QAction* action)
{
    openDatabase(action->data().toString());
}

void MainWindow::clearLastDatabases()
{
    config()->remove(Config::LastDatabases);
    bool inWelcomeWidget = (m_ui->stackedWidget->currentIndex() == 2);

    if (inWelcomeWidget) {
        m_ui->welcomeWidget->refreshLastDatabases();
    }
}

void MainWindow::openDatabase(const QString& filePath, const QString& password, const QString& keyfile)
{
    m_ui->tabWidget->addDatabaseTab(filePath, false, password, keyfile);
}

void MainWindow::setMenuActionState(DatabaseWidget::Mode mode)
{
    int currentIndex = m_ui->stackedWidget->currentIndex();

    bool inDatabaseTabWidget = (currentIndex == DatabaseTabScreen);
    bool inWelcomeWidget = (currentIndex == WelcomeScreen);
    bool inDatabaseTabWidgetOrWelcomeWidget = inDatabaseTabWidget || inWelcomeWidget;

    m_ui->actionDatabaseMerge->setEnabled(inDatabaseTabWidget);
    m_ui->actionDatabaseNew->setEnabled(inDatabaseTabWidgetOrWelcomeWidget);
    m_ui->actionDatabaseOpen->setEnabled(inDatabaseTabWidgetOrWelcomeWidget);
    m_ui->menuRecentDatabases->setEnabled(inDatabaseTabWidgetOrWelcomeWidget);
    m_ui->menuImport->setEnabled(inDatabaseTabWidgetOrWelcomeWidget);
    m_ui->actionLockDatabases->setEnabled(m_ui->tabWidget->hasLockableDatabases());

    if (inDatabaseTabWidget && m_ui->tabWidget->currentIndex() != -1) {
        DatabaseWidget* dbWidget = m_ui->tabWidget->currentDatabaseWidget();
        Q_ASSERT(dbWidget);

        if (mode == DatabaseWidget::Mode::None) {
            mode = dbWidget->currentMode();
        }

        switch (mode) {
        case DatabaseWidget::Mode::ViewMode: {
            bool singleEntrySelected = dbWidget->numberOfSelectedEntries() == 1;
            bool entriesSelected = dbWidget->numberOfSelectedEntries() > 0;
            bool groupSelected = dbWidget->isGroupSelected();
            bool currentGroupHasChildren = dbWidget->currentGroup()->hasChildren();
            bool currentGroupHasEntries = !dbWidget->currentGroup()->entries().isEmpty();
            bool recycleBinSelected = dbWidget->isRecycleBinSelected();
            bool sorted = dbWidget->isSorted();
            int entryIndex = dbWidget->currentEntryIndex();
            int numEntries = dbWidget->currentGroup()->entries().size();

            m_ui->actionEntryNew->setEnabled(true);
            m_ui->actionEntryClone->setEnabled(singleEntrySelected);
            m_ui->actionEntryEdit->setEnabled(singleEntrySelected);
            m_ui->actionEntryDelete->setEnabled(entriesSelected);
            m_ui->actionEntryMoveUp->setVisible(!sorted);
            m_ui->actionEntryMoveDown->setVisible(!sorted);
            m_ui->actionEntryMoveUp->setEnabled(singleEntrySelected && !sorted && entryIndex > 0);
            m_ui->actionEntryMoveDown->setEnabled(singleEntrySelected && !sorted && entryIndex >= 0
                                                  && entryIndex < numEntries - 1);
            m_ui->actionEntryCopyTitle->setEnabled(singleEntrySelected && dbWidget->currentEntryHasTitle());
            m_ui->actionEntryCopyUsername->setEnabled(singleEntrySelected && dbWidget->currentEntryHasUsername());
            // NOTE: Copy password is enabled even if the selected entry's password is blank to prevent Ctrl+C
            // from copying information from the currently selected cell in the entry view table.
            m_ui->actionEntryCopyPassword->setEnabled(singleEntrySelected);
            m_ui->actionEntryCopyURL->setEnabled(singleEntrySelected && dbWidget->currentEntryHasUrl());
            m_ui->actionEntryCopyNotes->setEnabled(singleEntrySelected && dbWidget->currentEntryHasNotes());
            m_ui->menuEntryCopyAttribute->setEnabled(singleEntrySelected);
            m_ui->menuEntryTotp->setEnabled(singleEntrySelected);
            m_ui->actionEntryAutoType->setEnabled(singleEntrySelected);
            m_ui->menuEntryAutoTypeWithSequence->setEnabled(singleEntrySelected);
            m_ui->actionEntryAutoTypeUsername->setEnabled(singleEntrySelected && dbWidget->currentEntryHasUsername());
            m_ui->actionEntryAutoTypeUsernameEnter->setEnabled(singleEntrySelected
                                                               && dbWidget->currentEntryHasUsername());
            m_ui->actionEntryAutoTypePassword->setEnabled(singleEntrySelected && dbWidget->currentEntryHasPassword());
            m_ui->actionEntryAutoTypePasswordEnter->setEnabled(singleEntrySelected
                                                               && dbWidget->currentEntryHasPassword());
            m_ui->actionEntryOpenUrl->setEnabled(singleEntrySelected && dbWidget->currentEntryHasUrl());
            m_ui->actionEntryTotp->setEnabled(singleEntrySelected && dbWidget->currentEntryHasTotp());
            m_ui->actionEntryCopyTotp->setEnabled(singleEntrySelected && dbWidget->currentEntryHasTotp());
            m_ui->actionEntrySetupTotp->setEnabled(singleEntrySelected);
            m_ui->actionEntryTotpQRCode->setEnabled(singleEntrySelected && dbWidget->currentEntryHasTotp());
            m_ui->actionEntryDownloadIcon->setEnabled((entriesSelected && !singleEntrySelected)
                                                      || (singleEntrySelected && dbWidget->currentEntryHasUrl()));
            m_ui->actionGroupNew->setEnabled(groupSelected);
            m_ui->actionGroupEdit->setEnabled(groupSelected);
            m_ui->actionGroupDelete->setEnabled(groupSelected && dbWidget->canDeleteCurrentGroup());
            m_ui->actionGroupSortAsc->setEnabled(groupSelected && currentGroupHasChildren);
            m_ui->actionGroupSortDesc->setEnabled(groupSelected && currentGroupHasChildren);
            m_ui->actionGroupEmptyRecycleBin->setVisible(recycleBinSelected);
            m_ui->actionGroupEmptyRecycleBin->setEnabled(recycleBinSelected);
            m_ui->actionGroupDownloadFavicons->setVisible(!recycleBinSelected);
            m_ui->actionGroupDownloadFavicons->setEnabled(groupSelected && currentGroupHasEntries
                                                          && !recycleBinSelected);
            m_ui->actionDatabaseSecurity->setEnabled(true);
            m_ui->actionReports->setEnabled(true);
            m_ui->actionDatabaseSettings->setEnabled(true);
            m_ui->actionDatabaseSave->setEnabled(m_ui->tabWidget->canSave());
            m_ui->actionDatabaseSaveAs->setEnabled(true);
            m_ui->actionDatabaseSaveBackup->setEnabled(true);
            m_ui->menuExport->setEnabled(true);
            m_ui->actionExportCsv->setEnabled(true);
            m_ui->actionExportHtml->setEnabled(true);
            m_ui->actionDatabaseMerge->setEnabled(m_ui->tabWidget->currentIndex() != -1);
#ifdef WITH_XC_SSHAGENT
            bool singleEntryHasSshKey =
                singleEntrySelected && sshAgent()->isEnabled() && dbWidget->currentEntryHasSshKey();
            m_ui->actionEntryAddToAgent->setVisible(singleEntryHasSshKey);
            m_ui->actionEntryAddToAgent->setEnabled(singleEntryHasSshKey);
            m_ui->actionEntryRemoveFromAgent->setVisible(singleEntryHasSshKey);
            m_ui->actionEntryRemoveFromAgent->setEnabled(singleEntryHasSshKey);
#endif

            m_searchWidgetAction->setEnabled(true);

            break;
        }
        case DatabaseWidget::Mode::EditMode:
        case DatabaseWidget::Mode::ImportMode:
        case DatabaseWidget::Mode::LockedMode: {
            // Enable select actions when editing an entry
            bool editEntryActive = dbWidget->isEntryEditActive();
            const auto editEntryActionsMask = QList<QAction*>({m_ui->actionEntryCopyUsername,
                                                               m_ui->actionEntryCopyPassword,
                                                               m_ui->actionEntryCopyURL,
                                                               m_ui->actionEntryOpenUrl,
                                                               m_ui->actionEntryAutoType,
                                                               m_ui->menuEntryAutoTypeWithSequence->menuAction(),
                                                               m_ui->actionEntryDownloadIcon,
                                                               m_ui->actionEntryCopyNotes,
                                                               m_ui->actionEntryCopyTitle,
                                                               m_ui->menuEntryCopyAttribute->menuAction(),
                                                               m_ui->menuEntryTotp->menuAction(),
                                                               m_ui->actionEntrySetupTotp});

            auto entryActions = m_ui->menuEntries->actions();
            entryActions << m_ui->menuEntryCopyAttribute->actions();
            entryActions << m_ui->menuEntryTotp->actions();
            for (auto action : entryActions) {
                bool enabled = editEntryActive && editEntryActionsMask.contains(action);
                if (action->menu()) {
                    action->menu()->setEnabled(enabled);
                }
                action->setEnabled(enabled);
            }

            const auto groupActions = m_ui->menuGroups->actions();
            for (auto action : groupActions) {
                action->setEnabled(false);
            }

            m_ui->actionDatabaseSecurity->setEnabled(false);
            m_ui->actionReports->setEnabled(false);
            m_ui->actionDatabaseSettings->setEnabled(false);
            m_ui->actionDatabaseSave->setEnabled(false);
            m_ui->actionDatabaseSaveAs->setEnabled(false);
            m_ui->actionDatabaseSaveBackup->setEnabled(false);
            m_ui->menuExport->setEnabled(false);
            m_ui->actionExportCsv->setEnabled(false);
            m_ui->actionExportHtml->setEnabled(false);
            m_ui->actionDatabaseMerge->setEnabled(false);

            m_searchWidgetAction->setEnabled(false);
            break;
        }
        default:
            Q_ASSERT(false);
        }
        m_ui->actionDatabaseClose->setEnabled(true);
    } else {
        const auto entryActions = m_ui->menuEntries->actions();
        for (auto action : entryActions) {
            action->setEnabled(false);
        }

        const auto groupActions = m_ui->menuGroups->actions();
        for (auto action : groupActions) {
            action->setEnabled(false);
        }

        m_ui->actionDatabaseSecurity->setEnabled(false);
        m_ui->actionReports->setEnabled(false);
        m_ui->actionDatabaseSettings->setEnabled(false);
        m_ui->actionDatabaseSave->setEnabled(false);
        m_ui->actionDatabaseSaveAs->setEnabled(false);
        m_ui->actionDatabaseSaveBackup->setEnabled(false);
        m_ui->actionDatabaseClose->setEnabled(false);
        m_ui->menuExport->setEnabled(false);
        m_ui->actionExportCsv->setEnabled(false);
        m_ui->actionExportHtml->setEnabled(false);
        m_ui->actionDatabaseMerge->setEnabled(false);

        m_searchWidgetAction->setEnabled(false);
    }

    if ((currentIndex == PasswordGeneratorScreen) != m_ui->actionPasswordGenerator->isChecked()) {
        bool blocked = m_ui->actionPasswordGenerator->blockSignals(true);
        m_ui->actionPasswordGenerator->toggle();
        m_ui->actionPasswordGenerator->blockSignals(blocked);
    } else if ((currentIndex == SettingsScreen) != m_ui->actionSettings->isChecked()) {
        bool blocked = m_ui->actionSettings->blockSignals(true);
        m_ui->actionSettings->toggle();
        m_ui->actionSettings->blockSignals(blocked);
    }
}

void MainWindow::updateToolbarSeparatorVisibility()
{
    if (!m_showToolbarSeparator) {
        m_ui->toolbarSeparator->setVisible(false);
        return;
    }

    switch (m_ui->stackedWidget->currentIndex()) {
    case DatabaseTabScreen:
        m_ui->toolbarSeparator->setVisible(!m_ui->tabWidget->tabBar()->isVisible()
                                           && m_ui->tabWidget->tabBar()->count() == 1);
        break;
    case SettingsScreen:
        m_ui->toolbarSeparator->setVisible(true);
        break;
    default:
        m_ui->toolbarSeparator->setVisible(false);
    }
}

void MainWindow::updateWindowTitle()
{
    QString customWindowTitlePart;
    int stackedWidgetIndex = m_ui->stackedWidget->currentIndex();
    int tabWidgetIndex = m_ui->tabWidget->currentIndex();
    bool isModified = m_ui->tabWidget->isModified(tabWidgetIndex);

    if (stackedWidgetIndex == DatabaseTabScreen && tabWidgetIndex != -1) {
        customWindowTitlePart = m_ui->tabWidget->tabName(tabWidgetIndex);
        if (isModified) {
            // remove asterisk '*' from title
            customWindowTitlePart.remove(customWindowTitlePart.size() - 1, 1);
        }
        m_ui->actionDatabaseSave->setEnabled(m_ui->tabWidget->canSave(tabWidgetIndex));
    } else if (stackedWidgetIndex == 1) {
        customWindowTitlePart = tr("Settings");
    }

    QString windowTitle;
    if (customWindowTitlePart.isEmpty()) {
        windowTitle = BaseWindowTitle;
    } else {
        windowTitle = QString("%1[*] - %2").arg(customWindowTitlePart, BaseWindowTitle);
    }

    if (customWindowTitlePart.isEmpty() || stackedWidgetIndex == 1) {
        setWindowFilePath("");
    } else {
        setWindowFilePath(m_ui->tabWidget->databaseWidgetFromIndex(tabWidgetIndex)->database()->filePath());
    }

    setWindowTitle(windowTitle);
    setWindowModified(isModified);
}

void MainWindow::showAboutDialog()
{
    auto* aboutDialog = new AboutDialog(this);
    aboutDialog->open();
}

void MainWindow::performUpdateCheck()
{
#ifdef WITH_XC_UPDATECHECK
    if (!config()->get(Config::UpdateCheckMessageShown).toBool()) {
        auto result =
            MessageBox::question(this,
                                 tr("Check for updates on startup?"),
                                 tr("Would you like KeePassXC to check for updates on startup?") + "\n\n"
                                     + tr("You can always check for updates manually from the application menu."),
                                 MessageBox::Yes | MessageBox::No,
                                 MessageBox::Yes);

        config()->set(Config::GUI_CheckForUpdates, (result == MessageBox::Yes));
        config()->set(Config::UpdateCheckMessageShown, true);
    }

    if (config()->get(Config::GUI_CheckForUpdates).toBool()) {
        updateCheck()->checkForUpdates(false);
    }

#endif
}

void MainWindow::hasUpdateAvailable(bool hasUpdate, const QString& version, bool isManuallyRequested)
{
#ifdef WITH_XC_UPDATECHECK
    if (hasUpdate && !isManuallyRequested) {
        auto* updateCheckDialog = new UpdateCheckDialog(this);
        updateCheckDialog->showUpdateCheckResponse(hasUpdate, version);
        updateCheckDialog->show();
    }
#else
    Q_UNUSED(hasUpdate)
    Q_UNUSED(version)
    Q_UNUSED(isManuallyRequested)
#endif
}

void MainWindow::showUpdateCheckDialog()
{
#ifdef WITH_XC_UPDATECHECK
    updateCheck()->checkForUpdates(true);
    auto* updateCheckDialog = new UpdateCheckDialog(this);
    updateCheckDialog->show();
#endif
}

void MainWindow::customOpenUrl(QString url)
{
    QDesktopServices::openUrl(QUrl(url));
}

void MainWindow::openDonateUrl()
{
    customOpenUrl("https://keepassxc.org/donate");
}

void MainWindow::openBugReportUrl()
{
    customOpenUrl("https://github.com/keepassxreboot/keepassxc/issues");
}

void MainWindow::openGettingStartedGuide()
{
    customOpenUrl(QString("file:///%1").arg(resources()->dataPath("docs/KeePassXC_GettingStarted.html")));
}

void MainWindow::openUserGuide()
{
    customOpenUrl(QString("file:///%1").arg(resources()->dataPath("docs/KeePassXC_UserGuide.html")));
}

void MainWindow::openOnlineHelp()
{
    customOpenUrl("https://keepassxc.org/docs/");
}

void MainWindow::openKeyboardShortcuts()
{
    customOpenUrl(QString("file:///%1").arg(resources()->dataPath("docs/KeePassXC_KeyboardShortcuts.html")));
}

void MainWindow::switchToDatabases()
{
    if (m_ui->tabWidget->currentIndex() == -1) {
        m_ui->stackedWidget->setCurrentIndex(WelcomeScreen);
    } else {
        m_ui->stackedWidget->setCurrentIndex(DatabaseTabScreen);
    }
}

void MainWindow::switchToSettings(bool enabled)
{
    if (enabled) {
        m_ui->settingsWidget->loadSettings();
        m_ui->stackedWidget->setCurrentIndex(SettingsScreen);
    } else {
        switchToDatabases();
    }
}

void MainWindow::togglePasswordGenerator(bool enabled)
{
    if (enabled) {
        m_ui->passwordGeneratorWidget->loadSettings();
        m_ui->passwordGeneratorWidget->regeneratePassword();
        m_ui->stackedWidget->setCurrentIndex(PasswordGeneratorScreen);
    } else {
        m_ui->passwordGeneratorWidget->saveSettings();
        switchToDatabases();
    }
}

void MainWindow::switchToNewDatabase()
{
    m_ui->tabWidget->newDatabase();
    switchToDatabases();
}

void MainWindow::switchToOpenDatabase()
{
    m_ui->tabWidget->openDatabase();
    switchToDatabases();
}

void MainWindow::switchToDatabaseFile(const QString& file)
{
    m_ui->tabWidget->addDatabaseTab(file);
    switchToDatabases();
}

void MainWindow::switchToKeePass1Database()
{
    m_ui->tabWidget->importKeePass1Database();
    switchToDatabases();
}

void MainWindow::switchToOpVaultDatabase()
{
    m_ui->tabWidget->importOpVaultDatabase();
    switchToDatabases();
}

void MainWindow::switchToCsvImport()
{
    m_ui->tabWidget->importCsv();
    switchToDatabases();
}

void MainWindow::databaseStatusChanged(DatabaseWidget* dbWidget)
{
    Q_UNUSED(dbWidget);
    updateTrayIcon();
}

/**
 * Select a database tab by its index. Stays bounded to first/last tab
 * on overflow unless wrap is true.
 *
 * @param tabIndex 0-based tab index selector
 * @param wrap if true wrap around to first/last tab
 */
void MainWindow::selectDatabaseTab(int tabIndex, bool wrap)
{
    if (m_ui->stackedWidget->currentIndex() == DatabaseTabScreen) {
        if (wrap) {
            if (tabIndex < 0) {
                tabIndex = m_ui->tabWidget->count() - 1;
            } else if (tabIndex >= m_ui->tabWidget->count()) {
                tabIndex = 0;
            }
        } else {
            tabIndex = qBound(0, tabIndex, m_ui->tabWidget->count() - 1);
        }

        m_ui->tabWidget->setCurrentIndex(tabIndex);
    }
}

void MainWindow::selectNextDatabaseTab()
{
    selectDatabaseTab(m_ui->tabWidget->currentIndex() + 1, true);
}

void MainWindow::selectPreviousDatabaseTab()
{
    selectDatabaseTab(m_ui->tabWidget->currentIndex() - 1, true);
}

void MainWindow::databaseTabChanged(int tabIndex)
{
    if (tabIndex != -1 && m_ui->stackedWidget->currentIndex() == WelcomeScreen) {
        m_ui->stackedWidget->setCurrentIndex(DatabaseTabScreen);
    } else if (tabIndex == -1 && m_ui->stackedWidget->currentIndex() == DatabaseTabScreen) {
        m_ui->stackedWidget->setCurrentIndex(WelcomeScreen);
    }

    m_actionMultiplexer.setCurrentObject(m_ui->tabWidget->currentDatabaseWidget());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_appExiting) {
        event->accept();
        return;
    }

    // Ignore event and hide to tray if this is not an actual close
    // request by the system's session manager.
    if (config()->get(Config::GUI_MinimizeOnClose).toBool() && !m_appExitCalled && !isHidden()
        && !qApp->isSavingSession()) {
        event->ignore();
        hideWindow();
        return;
    }

    m_appExiting = saveLastDatabases();
    if (m_appExiting) {
        saveWindowInformation();
        event->accept();
        m_restartRequested ? kpxcApp->restart() : QApplication::quit();
        return;
    }

    m_appExitCalled = false;
    m_restartRequested = false;
    event->ignore();
}

void MainWindow::changeEvent(QEvent* event)
{
    if ((event->type() == QEvent::WindowStateChange) && isMinimized()) {
        if (isTrayIconEnabled() && m_trayIcon && m_trayIcon->isVisible()
            && config()->get(Config::GUI_MinimizeToTray).toBool()) {
            event->ignore();
            hide();
        }

        if (config()->get(Config::Security_LockDatabaseMinimize).toBool()) {
            m_ui->tabWidget->lockDatabases();
        }
    } else {
        QMainWindow::changeEvent(event);
    }
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (!event->modifiers()) {
        // Allow for direct focus of search, group view, and entry view
        auto dbWidget = m_ui->tabWidget->currentDatabaseWidget();
        if (dbWidget && dbWidget->isEntryViewActive()) {
            if (event->key() == Qt::Key_F1) {
                dbWidget->focusOnGroups(true);
                return;
            } else if (event->key() == Qt::Key_F2) {
                dbWidget->focusOnEntries(true);
                return;
            } else if (event->key() == Qt::Key_F3) {
                m_searchWidget->searchFocus();
                return;
            }
        }
    }

    QWidget::keyPressEvent(event);
}

bool MainWindow::focusNextPrevChild(bool next)
{
    // Only navigate around the main window if the database widget is showing the entry view
    auto dbWidget = m_ui->tabWidget->currentDatabaseWidget();
    if (dbWidget && dbWidget->isVisible() && dbWidget->isEntryViewActive()) {
        // Search Widget <-> Tab Widget <-> DbWidget
        if (next) {
            if (m_searchWidget->hasFocus()) {
                m_ui->tabWidget->setFocus(Qt::TabFocusReason);
            } else if (m_ui->tabWidget->hasFocus()) {
                dbWidget->setFocus(Qt::TabFocusReason);
            } else {
                m_searchWidget->setFocus(Qt::TabFocusReason);
            }
        } else {
            if (m_searchWidget->hasFocus()) {
                dbWidget->setFocus(Qt::BacktabFocusReason);
            } else if (m_ui->tabWidget->hasFocus()) {
                m_searchWidget->setFocus(Qt::BacktabFocusReason);
            } else {
                m_ui->tabWidget->setFocus(Qt::BacktabFocusReason);
            }
        }
        return true;
    }

    // Defer to Qt to make a decision, this maintains normal behavior
    return QMainWindow::focusNextPrevChild(next);
}

void MainWindow::saveWindowInformation()
{
    if (isVisible()) {
        config()->set(Config::GUI_MainWindowGeometry, saveGeometry());
        config()->set(Config::GUI_MainWindowState, saveState());
    }
}

bool MainWindow::saveLastDatabases()
{
    if (config()->get(Config::OpenPreviousDatabasesOnStartup).toBool()) {
        auto currentDbWidget = m_ui->tabWidget->currentDatabaseWidget();
        if (currentDbWidget) {
            config()->set(Config::LastActiveDatabase, currentDbWidget->database()->filePath());
        } else {
            config()->remove(Config::LastActiveDatabase);
        }

        QStringList openDatabases;
        for (int i = 0; i < m_ui->tabWidget->count(); ++i) {
            auto dbWidget = m_ui->tabWidget->databaseWidgetFromIndex(i);
            openDatabases.append(dbWidget->database()->filePath());
        }

        config()->set(Config::LastOpenedDatabases, openDatabases);
    } else {
        config()->remove(Config::LastActiveDatabase);
        config()->remove(Config::LastOpenedDatabases);
    }

    return m_ui->tabWidget->closeAllDatabaseTabs();
}

void MainWindow::updateTrayIcon()
{
    if (isTrayIconEnabled()) {
        QApplication::setQuitOnLastWindowClosed(false);

        if (!m_trayIcon) {
            m_trayIcon = new QSystemTrayIcon(this);
            auto* menu = new QMenu(this);

            auto* actionToggle = new QAction(tr("Toggle window"), menu);
            menu->addAction(actionToggle);
            actionToggle->setIcon(icons()->icon("keepassxc-monochrome-dark"));

            menu->addAction(m_ui->actionLockDatabases);

#ifdef Q_OS_MACOS
            QAction* actionQuit = new QAction(tr("Quit KeePassXC"), menu);
            menu->addAction(actionQuit);

            connect(actionQuit, SIGNAL(triggered()), SLOT(appExit()));
#else
            menu->addAction(m_ui->actionQuit);

#endif
            connect(m_trayIcon,
                    SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
                    SLOT(trayIconTriggered(QSystemTrayIcon::ActivationReason)));
            connect(actionToggle, SIGNAL(triggered()), SLOT(toggleWindow()));

            m_trayIcon->setContextMenu(menu);

            m_trayIcon->setIcon(icons()->trayIcon());
            m_trayIcon->show();
        }

        if (m_ui->tabWidget->count() == 0) {
            m_trayIcon->setIcon(icons()->trayIcon());
        } else if (m_ui->tabWidget->hasLockableDatabases()) {
            m_trayIcon->setIcon(icons()->trayIconUnlocked());
        } else {
            m_trayIcon->setIcon(icons()->trayIconLocked());
        }
    } else {
        QApplication::setQuitOnLastWindowClosed(true);

        if (m_trayIcon) {
            m_trayIcon->hide();
            delete m_trayIcon;
        }
    }
}

void MainWindow::obtainContextFocusLock()
{
    m_contextMenuFocusLock = true;
}

void MainWindow::releaseContextFocusLock()
{
    m_contextMenuFocusLock = false;
}

void MainWindow::agentEnabled(bool enabled)
{
    m_ui->actionEntryAddToAgent->setVisible(enabled);
    m_ui->actionEntryRemoveFromAgent->setVisible(enabled);
}

void MainWindow::showEntryContextMenu(const QPoint& globalPos)
{
    bool entrySelected = false;
    auto dbWidget = m_ui->tabWidget->currentDatabaseWidget();
    if (dbWidget) {
        entrySelected = dbWidget->numberOfSelectedEntries() > 0;
    }

    if (entrySelected) {
        m_entryContextMenu->popup(globalPos);
    } else {
        m_entryNewContextMenu->popup(globalPos);
    }
}

void MainWindow::showGroupContextMenu(const QPoint& globalPos)
{
    m_ui->menuGroups->popup(globalPos);
}

void MainWindow::setShortcut(QAction* action, QKeySequence::StandardKey standard, int fallback)
{
    if (!QKeySequence::keyBindings(standard).isEmpty()) {
        action->setShortcuts(standard);
    } else if (fallback != 0) {
        action->setShortcut(QKeySequence(fallback));
    }
}

void MainWindow::applySettingsChanges()
{
    int timeout = config()->get(Config::Security_LockDatabaseIdleSeconds).toInt() * 1000;
    if (timeout <= 0) {
        timeout = 60;
    }

    m_inactivityTimer->setInactivityTimeout(timeout);
    if (config()->get(Config::Security_LockDatabaseIdle).toBool()) {
        m_inactivityTimer->activate();
    } else {
        m_inactivityTimer->deactivate();
    }

#ifdef WITH_XC_TOUCHID
    if (config()->get(Config::Security_ResetTouchId).toBool()) {
        // Calculate TouchID timeout in milliseconds
        timeout = config()->get(Config::Security_ResetTouchIdTimeout).toInt() * 60 * 1000;
        if (timeout <= 0) {
            timeout = 30 * 60 * 1000;
        }

        m_touchIDinactivityTimer->setInactivityTimeout(timeout);
        m_touchIDinactivityTimer->activate();
    } else {
        m_touchIDinactivityTimer->deactivate();
    }
#endif

    m_ui->toolBar->setHidden(config()->get(Config::GUI_HideToolbar).toBool());
    m_ui->toolBar->setMovable(config()->get(Config::GUI_MovableToolbar).toBool());

    bool isOk = false;
    const auto toolButtonStyle =
        static_cast<Qt::ToolButtonStyle>(config()->get(Config::GUI_ToolButtonStyle).toInt(&isOk));
    if (isOk) {
        m_ui->toolBar->setToolButtonStyle(toolButtonStyle);
    }

    updateTrayIcon();
}

void MainWindow::focusWindowChanged(QWindow* focusWindow)
{
    if (focusWindow != windowHandle()) {
        m_lastFocusOutTime = Clock::currentMilliSecondsSinceEpoch();
    }
}

void MainWindow::trayIconTriggered(QSystemTrayIcon::ActivationReason reason)
{
    if (!m_trayIconTriggerTimer.isActive()) {
        m_trayIconTriggerTimer.start(150);
    }
    // Overcome Qt bug https://bugreports.qt.io/browse/QTBUG-69698
    // Store last issued tray icon activation reason to properly
    // capture doubleclick events
    m_trayIconTriggerReason = reason;
}

void MainWindow::processTrayIconTrigger()
{
#ifdef Q_OS_MACOS
    // Do not toggle the window on macOS and just show the context menu instead.
    // Right click detection doesn't seem to be working anyway
    // and anything else will only trigger the context menu AND
    // toggle the window at the same time, which is confusing at best.
    // Showing only a context menu for tray icons seems to be best
    // practice on macOS anyway, so this is probably fine.
    return;
#endif

    if (m_trayIconTriggerReason == QSystemTrayIcon::DoubleClick) {
        // Always toggle window on double click
        toggleWindow();
    } else if (m_trayIconTriggerReason == QSystemTrayIcon::Trigger
               || m_trayIconTriggerReason == QSystemTrayIcon::MiddleClick) {
        // Toggle window if is not in front.
#ifdef Q_OS_WIN
        // If on Windows, check if focus switched within the 500 milliseconds since
        // clicking the tray icon removes focus from main window.
        if (isHidden() || (Clock::currentMilliSecondsSinceEpoch() - m_lastFocusOutTime) <= 500) {
#else
        // If on Linux, check if the window has focus.
        if (hasFocus() || isHidden() || windowHandle()->isActive()) {
#endif
            toggleWindow();
        } else {
            bringToFront();
        }
    }
}

void MainWindow::show()
{
#ifndef Q_OS_WIN
    m_lastShowTime = Clock::currentMilliSecondsSinceEpoch();
#endif
#ifdef Q_OS_MACOS
    // Unset minimize state to avoid weird fly-in effects
    setWindowState(windowState() & ~Qt::WindowMinimized);
    macUtils()->toggleForegroundApp(true);
#endif
    QMainWindow::show();
}

void MainWindow::hide()
{
#ifndef Q_OS_WIN
    qint64 current_time = Clock::currentMilliSecondsSinceEpoch();
    if (current_time - m_lastShowTime < 50) {
        return;
    }
#endif
    QMainWindow::hide();
#ifdef Q_OS_MACOS
    macUtils()->toggleForegroundApp(false);
#endif
}

void MainWindow::hideWindow()
{
    saveWindowInformation();

    // Only hide if tray icon is active, otherwise window will be gone forever
    if (isTrayIconEnabled()) {
        // On X11, the window should NOT be minimized and hidden at the same time. See issue #1595.
        // On macOS, we are skipping minimization as well to avoid playing the magic lamp animation.
        if (QGuiApplication::platformName() != "xcb" && QGuiApplication::platformName() != "cocoa") {
            setWindowState(windowState() | Qt::WindowMinimized);
        }
        hide();
    } else {
        showMinimized();
    }

    if (config()->get(Config::Security_LockDatabaseMinimize).toBool()) {
        m_ui->tabWidget->lockDatabases();
    }
}

void MainWindow::minimizeOrHide()
{
    if (config()->get(Config::GUI_MinimizeToTray).toBool()) {
        hideWindow();
    } else {
        showMinimized();
    }
}

void MainWindow::toggleWindow()
{
    if (isVisible() && !isMinimized()) {
        hideWindow();
    } else {
        bringToFront();

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS) && !defined(QT_NO_DBUS) && (QT_VERSION < QT_VERSION_CHECK(5, 9, 0))
        // re-register global D-Bus menu (needed on Ubuntu with Unity)
        // see https://github.com/keepassxreboot/keepassxc/issues/271
        // and https://bugreports.qt.io/browse/QTBUG-58723
        // check for !isVisible(), because isNativeMenuBar() does not work with appmenu-qt5
        static const auto isDesktopSessionUnity = qgetenv("XDG_CURRENT_DESKTOP") == "Unity";

        if (isDesktopSessionUnity && Tools::qtRuntimeVersion() < QT_VERSION_CHECK(5, 9, 0)
            && !m_ui->menubar->isVisible()) {
            QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("com.canonical.AppMenu.Registrar"),
                                                              QStringLiteral("/com/canonical/AppMenu/Registrar"),
                                                              QStringLiteral("com.canonical.AppMenu.Registrar"),
                                                              QStringLiteral("RegisterWindow"));
            QList<QVariant> args;
            args << QVariant::fromValue(static_cast<uint32_t>(winId()))
                 << QVariant::fromValue(QDBusObjectPath("/MenuBar/1"));
            msg.setArguments(args);
            QDBusConnection::sessionBus().send(msg);
        }
#endif
    }
}

void MainWindow::lockDatabasesAfterInactivity()
{
    // ignore event if a modal dialog is open (such as a message box or file dialog)
    if (QApplication::activeModalWidget()) {
        return;
    }

    m_ui->tabWidget->lockDatabases();
}

void MainWindow::forgetTouchIDAfterInactivity()
{
#ifdef WITH_XC_TOUCHID
    TouchID::getInstance().reset();
#endif
}

bool MainWindow::isTrayIconEnabled() const
{
    return config()->get(Config::GUI_ShowTrayIcon).toBool() && QSystemTrayIcon::isSystemTrayAvailable();
}

void MainWindow::displayGlobalMessage(const QString& text,
                                      MessageWidget::MessageType type,
                                      bool showClosebutton,
                                      int autoHideTimeout)
{
    m_ui->globalMessageWidget->setCloseButtonVisible(showClosebutton);
    m_ui->globalMessageWidget->showMessage(text, type, autoHideTimeout);
}

void MainWindow::displayTabMessage(const QString& text,
                                   MessageWidget::MessageType type,
                                   bool showClosebutton,
                                   int autoHideTimeout)
{
    m_ui->tabWidget->currentDatabaseWidget()->showMessage(text, type, showClosebutton, autoHideTimeout);
}

void MainWindow::hideGlobalMessage()
{
    m_ui->globalMessageWidget->hideMessage();
}

void MainWindow::showYubiKeyPopup()
{
    displayGlobalMessage(tr("Please touch the button on your YubiKey!"),
                         MessageWidget::Information,
                         false,
                         MessageWidget::DisableAutoHide);
    setEnabled(false);
}

void MainWindow::hideYubiKeyPopup()
{
    hideGlobalMessage();
    setEnabled(true);
}

void MainWindow::bringToFront()
{
    ensurePolished();
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    show();
    raise();
    activateWindow();
}

void MainWindow::handleScreenLock()
{
    if (config()->get(Config::Security_LockDatabaseScreenLock).toBool()) {
        lockDatabasesAfterInactivity();
    }

#ifdef WITH_XC_TOUCHID
    if (config()->get(Config::Security_ResetTouchIdScreenlock).toBool()) {
        forgetTouchIDAfterInactivity();
    }
#endif
}

QStringList MainWindow::kdbxFilesFromUrls(const QList<QUrl>& urls)
{
    QStringList kdbxFiles;
    for (const QUrl& url : urls) {
        const QFileInfo fInfo(url.toLocalFile());
        const bool isKdbxFile = fInfo.isFile() && fInfo.suffix().toLower() == "kdbx";
        if (isKdbxFile) {
            kdbxFiles.append(fInfo.absoluteFilePath());
        }
    }

    return kdbxFiles;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    const QMimeData* mimeData = event->mimeData();
    if (mimeData->hasUrls()) {
        const QStringList kdbxFiles = kdbxFilesFromUrls(mimeData->urls());
        if (!kdbxFiles.isEmpty()) {
            event->acceptProposedAction();
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const QMimeData* mimeData = event->mimeData();
    if (mimeData->hasUrls()) {
        const QStringList kdbxFiles = kdbxFilesFromUrls(mimeData->urls());
        if (!kdbxFiles.isEmpty()) {
            event->acceptProposedAction();
        }
        for (const QString& kdbxFile : kdbxFiles) {
            openDatabase(kdbxFile);
        }
    }
}

void MainWindow::closeAllDatabases()
{
    m_ui->tabWidget->closeAllDatabaseTabs();
}

void MainWindow::lockAllDatabases()
{
    lockDatabasesAfterInactivity();
}

void MainWindow::displayDesktopNotification(const QString& msg, QString title, int msTimeoutHint)
{
    if (!m_trayIcon || !QSystemTrayIcon::supportsMessages()) {
        return;
    }

    if (title.isEmpty()) {
        title = BaseWindowTitle;
    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    m_trayIcon->showMessage(title, msg, icons()->applicationIcon(), msTimeoutHint);
#else
    m_trayIcon->showMessage(title, msg, QSystemTrayIcon::Information, msTimeoutHint);
#endif
}

void MainWindow::restartApp(const QString& message)
{
    auto ans = MessageBox::question(
        this, tr("Restart Application?"), message, MessageBox::Yes | MessageBox::No, MessageBox::Yes);
    if (ans == MessageBox::Yes) {
        m_appExitCalled = true;
        m_restartRequested = true;
        close();
    } else {
        m_restartRequested = false;
    }
}

void MainWindow::initViewMenu()
{
    m_ui->actionThemeAuto->setData("auto");
    m_ui->actionThemeLight->setData("light");
    m_ui->actionThemeDark->setData("dark");
    m_ui->actionThemeClassic->setData("classic");

    auto themeActions = new QActionGroup(this);
    themeActions->addAction(m_ui->actionThemeAuto);
    themeActions->addAction(m_ui->actionThemeLight);
    themeActions->addAction(m_ui->actionThemeDark);
    themeActions->addAction(m_ui->actionThemeClassic);

    auto theme = config()->get(Config::GUI_ApplicationTheme).toString();
    for (auto action : themeActions->actions()) {
        if (action->data() == theme) {
            action->setChecked(true);
            break;
        }
    }

    connect(themeActions, &QActionGroup::triggered, this, [this, theme](QAction* action) {
        config()->set(Config::GUI_ApplicationTheme, action->data());
        if ((action->data() == "classic" || theme == "classic") && action->data() != theme) {
            restartApp(tr("You must restart the application to apply this setting. Would you like to restart now?"));
        } else {
            kpxcApp->applyTheme();
        }
    });

    bool compact = config()->get(Config::GUI_CompactMode).toBool();
    m_ui->actionCompactMode->setChecked(compact);
    connect(m_ui->actionCompactMode, &QAction::toggled, this, [this, compact](bool checked) {
        config()->set(Config::GUI_CompactMode, checked);
        if (checked != compact) {
            restartApp(tr("You must restart the application to apply this setting. Would you like to restart now?"));
        }
    });

    m_ui->actionShowToolbar->setChecked(!config()->get(Config::GUI_HideToolbar).toBool());
    connect(m_ui->actionShowToolbar, &QAction::toggled, this, [this](bool checked) {
        config()->set(Config::GUI_HideToolbar, !checked);
        applySettingsChanges();
    });

    m_ui->actionShowGroupsPanel->setChecked(!config()->get(Config::GUI_HideGroupsPanel).toBool());
    connect(m_ui->actionShowGroupsPanel, &QAction::toggled, this, [](bool checked) {
        config()->set(Config::GUI_HideGroupsPanel, !checked);
    });

    m_ui->actionShowPreviewPanel->setChecked(!config()->get(Config::GUI_HidePreviewPanel).toBool());
    connect(m_ui->actionShowPreviewPanel, &QAction::toggled, this, [](bool checked) {
        config()->set(Config::GUI_HidePreviewPanel, !checked);
    });

    connect(m_ui->actionAlwaysOnTop, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
        } else {
            setWindowFlags(windowFlags() & ~Qt::WindowStaysOnTopHint);
        }
        show();
    });

    m_ui->actionHideUsernames->setChecked(config()->get(Config::GUI_HideUsernames).toBool());
    connect(m_ui->actionHideUsernames, &QAction::toggled, this, [](bool checked) {
        config()->set(Config::GUI_HideUsernames, checked);
    });

    m_ui->actionHidePasswords->setChecked(config()->get(Config::GUI_HidePasswords).toBool());
    connect(m_ui->actionHidePasswords, &QAction::toggled, this, [](bool checked) {
        config()->set(Config::GUI_HidePasswords, checked);
    });
}

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)

MainWindowEventFilter::MainWindowEventFilter(QObject* parent)
    : QObject(parent)
{
}

/**
 * MainWindow event filter to initiate empty-area drag on the toolbar, menubar, and tabbar.
 */
bool MainWindowEventFilter::eventFilter(QObject* watched, QEvent* event)
{
    auto* mainWindow = getMainWindow();
    if (!mainWindow || !mainWindow->m_ui) {
        return QObject::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseButtonPress) {
        if (watched == mainWindow->m_ui->menubar) {
            mainWindow->windowHandle()->startSystemMove();
            // Continue processing events, so menus keep working.
            return false;
        } else if (watched == mainWindow->m_ui->toolBar) {
            if (!mainWindow->m_ui->toolBar->isMovable() || mainWindow->m_ui->toolBar->cursor() != Qt::SizeAllCursor) {
                mainWindow->windowHandle()->startSystemMove();
                return false;
            }
        } else if (watched == mainWindow->m_ui->tabWidget->tabBar()) {
            auto* m = static_cast<QMouseEvent*>(event);
            if (mainWindow->m_ui->tabWidget->tabBar()->tabAt(m->pos()) == -1) {
                mainWindow->windowHandle()->startSystemMove();
                return true;
            }
        }
    }

    return QObject::eventFilter(watched, event);
}

#endif
