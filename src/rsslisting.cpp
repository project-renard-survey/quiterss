#include <QtCore>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <Psapi.h>
#endif

#include "aboutdialog.h"
#include "addfeedwizard.h"
#include "db_func.h"
#include "delegatewithoutfocus.h"
#include "feedpropertiesdialog.h"
#include "filterrulesdialog.h"
#include "newsfiltersdialog.h"
#include "optionsdialog.h"
#include "rsslisting.h"
#include "VersionNo.h"
#include "webpage.h"

/*!****************************************************************************/
RSSListing::RSSListing(QSettings *settings, QString dataDirPath, QWidget *parent)
  : QMainWindow(parent),
    settings_(settings),
    dataDirPath_(dataDirPath)
{
  setWindowTitle(QString("QuiteRSS v") + QString(STRFILEVER).section('.', 0, 2));
  setContextMenuPolicy(Qt::CustomContextMenu);

  dbFileName_ = dataDirPath_ + QDir::separator() + kDbName;
  initDB(dbFileName_);

  db_ = QSqlDatabase::addDatabase("QSQLITE");
  db_.setDatabaseName(":memory:");
  db_.open();

  dbMemFileThread_ = new DBMemFileThread(this);
  dbMemFileThread_->sqliteDBMemFile(db_, dbFileName_, false);
  dbMemFileThread_->start(QThread::NormalPriority);
  while(dbMemFileThread_->isRunning()) qApp->processEvents();

  persistentUpdateThread_ = new UpdateThread(this);
  persistentUpdateThread_->setObjectName("persistentUpdateThread_");
  connect(this, SIGNAL(startGetUrlTimer()),
          persistentUpdateThread_, SIGNAL(startGetUrlTimer()));
  connect(persistentUpdateThread_, SIGNAL(readedXml(QByteArray, QUrl)),
          this, SLOT(receiveXml(QByteArray, QUrl)));
  connect(persistentUpdateThread_, SIGNAL(getUrlDone(int,QDateTime)),
          this, SLOT(getUrlDone(int,QDateTime)));

  persistentParseThread_ = new ParseThread(this, &db_);
  persistentParseThread_->setObjectName("persistentParseThread_");
  connect(this, SIGNAL(xmlReadyParse(QByteArray,QUrl)),
          persistentParseThread_, SLOT(parseXml(QByteArray,QUrl)),
          Qt::QueuedConnection);

  createFeedsDock();
  createNewsDock();
  createToolBarNull();
  createWebWidget();

  createActions();
  createShortcut();
  createMenu();
  createToolBar();
  createMenuNews();
  createMenuFeed();

  createStatusBar();
  createTray();

  connect(this, SIGNAL(signalCloseApp()),
          SLOT(slotCloseApp()), Qt::QueuedConnection);
  commitDataRequest_ = false;
  connect(qApp, SIGNAL(commitDataRequest(QSessionManager&)),
          this, SLOT(slotCommitDataRequest(QSessionManager&)));

  faviconLoader = new FaviconLoader(this);
  connect(this, SIGNAL(startGetUrlTimer()),
          faviconLoader, SIGNAL(startGetUrlTimer()));
  connect(faviconLoader, SIGNAL(signalIconRecived(const QString&, const QByteArray &)),
          this, SLOT(slotIconFeedLoad(const QString&, const QByteArray &)));

  loadSettingsFeeds();

  readSettings();

  if (autoUpdatefeedsStartUp_) slotGetAllFeeds();
  int updateFeedsTime = autoUpdatefeedsTime_*60000;
  if (autoUpdatefeedsInterval_ == 1)
    updateFeedsTime = updateFeedsTime*60;
  updateFeedsTimer_.start(updateFeedsTime, this);

  QTimer::singleShot(10000, this, SLOT(slotUpdateAppChacking()));

  translator_ = new QTranslator(this);
  appInstallTranslator();
}

/*!****************************************************************************/
RSSListing::~RSSListing()
{
  qDebug("App_Closing");

  persistentUpdateThread_->quit();
  persistentParseThread_->quit();
  faviconLoader->quit();

  db_.transaction();
  QSqlQuery q(db_);
  q.exec("SELECT id FROM feeds");
  while (q.next()) {
    QSqlQuery qt(db_);
    QString qStr = QString("UPDATE news SET new=0 WHERE feedId=='%1' AND new=1")
        .arg(q.value(0).toString());
    qt.exec(qStr);
    qStr = QString("UPDATE news SET read=2 WHERE feedId=='%1' AND read=1").
        arg(q.value(0).toString());
    qt.exec(qStr);

    feedsCleanUp(q.value(0).toString());

    qStr = QString("UPDATE news SET title='', published='' "
        "WHERE feedId=='%1' AND deleted=1 AND guid!=''").
        arg(q.value(0).toString());
    qt.exec(qStr);

    qStr = QString("UPDATE news "
        "SET description='', content='', received='', author_name='', "
        "link_href='', category='', deleted=2 "
        "WHERE feedId=='%1' AND deleted=1").
        arg(q.value(0).toString());
    qt.exec(qStr);
  }
  db_.commit();

  q.exec("UPDATE feeds SET newCount=0");
  q.exec("VACUUM");

  dbMemFileThread_->sqliteDBMemFile(db_, dbFileName_, true);
  dbMemFileThread_->start();
  while(dbMemFileThread_->isRunning());

  while (persistentUpdateThread_->isRunning());
  while (persistentParseThread_->isRunning());
  while (faviconLoader->isRunning());

  db_.close();

  QSqlDatabase::removeDatabase(QString());
}

void RSSListing::slotCommitDataRequest(QSessionManager &manager)
{
  manager.release();
  commitDataRequest_ = true;
}

/*virtual*/ void RSSListing::showEvent(QShowEvent*)
{
  connect(feedsDock_, SIGNAL(dockLocationChanged(Qt::DockWidgetArea)),
          this, SLOT(slotDockLocationChanged(Qt::DockWidgetArea)), Qt::UniqueConnection);
}

/*!****************************************************************************/
bool RSSListing::eventFilter(QObject *obj, QEvent *event)
{
  if (obj == feedsView_->viewport()) {
    if (event->type() == QEvent::ToolTip) {
      return true;
    }
    return false;
  } else if (obj == toolBarNull_) {
    if (event->type() == QEvent::MouseButtonRelease) {
      slotVisibledFeedsDock();
    }
    return false;
  } else {
    // pass the event on to the parent class
    return QMainWindow::eventFilter(obj, event);
  }
}

/*! \brief ОБработка событий закрытия окна ************************************/
/*virtual*/ void RSSListing::closeEvent(QCloseEvent* event)
{
  event->ignore();
  if (closingTray_ && !commitDataRequest_ && showTrayIcon_) {
    oldState = windowState();
    emit signalPlaceToTray();
  } else {
    slotClose();
  }
}

/*! \brief Обработка события выхода из приложения *****************************/
void RSSListing::slotClose()
{
  traySystem->hide();
  hide();
  writeSettings();
  saveActionShortcuts();
  emit signalCloseApp();
}

/*! \brief Завершение приложения **********************************************/
void RSSListing::slotCloseApp()
{
  qApp->quit();
}

/*! \brief Обработка события изменения состояния окна *************************/
/*virtual*/ void RSSListing::changeEvent(QEvent *event)
{
  if(event->type() == QEvent::WindowStateChange) {
    if(isMinimized()) {
      oldState = ((QWindowStateChangeEvent*)event)->oldState();
      if (minimizingTray_ && showTrayIcon_) {
        event->ignore();
        emit signalPlaceToTray();
        return;
      }
    } else {
      oldState = windowState();
    }
  } else if(event->type() == QEvent::ActivationChange) {
    if (isActiveWindow() && (behaviorIconTray_ == 1))
      traySystem->setIcon(QIcon(":/images/quiterss16"));
  } else if(event->type() == QEvent::LanguageChange) {
    retranslateStrings();
  }
  QMainWindow::changeEvent(event);
}

/*! \brief Обработка события помещения программы в трей ***********************/
void RSSListing::slotPlaceToTray()
{
  hide();
  if (emptyWorking_)
    QTimer::singleShot(10000, this, SLOT(myEmptyWorkingSet()));
  if (clearStatusNew_)
    markAllFeedsRead(false);

  dbMemFileThread_->sqliteDBMemFile(db_, dbFileName_, true);
  dbMemFileThread_->start(QThread::LowestPriority);
  writeSettings();
}

/*! \brief Обработка событий трея *********************************************/
void RSSListing::slotActivationTray(QSystemTrayIcon::ActivationReason reason)
{
  switch (reason) {
  case QSystemTrayIcon::Unknown:
    break;
  case QSystemTrayIcon::Context:
    trayMenu_->activateWindow();
    break;
  case QSystemTrayIcon::DoubleClick:
    if (!singleClickTray_) slotShowWindows(true);
    break;
  case QSystemTrayIcon::Trigger:
    if (singleClickTray_) slotShowWindows(true);
    break;
  case QSystemTrayIcon::MiddleClick:
    break;
  }
}

/*! \brief Отображение окна по событию ****************************************/
void RSSListing::slotShowWindows(bool trayClick)
{
  if (!trayClick || isHidden()){
    if (oldState == Qt::WindowMaximized) {
      showMaximized();
    } else {
      showNormal();
    }
    activateWindow();
  } else {
    emit signalPlaceToTray();
  }
}

void RSSListing::timerEvent(QTimerEvent *event)
{
  if (event->timerId() == updateFeedsTimer_.timerId()) {
    updateFeedsTimer_.stop();
    int updateFeedsTime = autoUpdatefeedsTime_*60000;
    if (autoUpdatefeedsInterval_ == 1)
      updateFeedsTime = updateFeedsTime*60;
    updateFeedsTimer_.start(updateFeedsTime, this);
    slotTimerUpdateFeeds();
  } else if (event->timerId() == markNewsReadTimer_.timerId()) {
    markNewsReadTimer_.stop();
    slotSetItemRead(newsView_->currentIndex(), 1);
  }
}

void RSSListing::createFeedsDock()
{
  feedsModel_ = new FeedsModel(this);
  feedsModel_->setTable("feeds");
  feedsModel_->select();

  feedsView_ = new FeedsView(this);
  feedsView_->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  feedsView_->setModel(feedsModel_);
  for (int i = 0; i < feedsModel_->columnCount(); ++i)
    feedsView_->hideColumn(i);
  feedsView_->showColumn(feedsModel_->fieldIndex("text"));
  feedsView_->showColumn(feedsModel_->fieldIndex("unread"));
  feedsView_->header()->setResizeMode(feedsModel_->fieldIndex("text"), QHeaderView::Stretch);
  feedsView_->header()->setResizeMode(feedsModel_->fieldIndex("unread"), QHeaderView::ResizeToContents);

  //! Create title DockWidget
  feedsTitleLabel_ = new QLabel(this);
  feedsTitleLabel_->setObjectName("feedsTitleLabel_");
  feedsTitleLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
  feedsTitleLabel_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);

  feedsToolBar_ = new QToolBar(this);
  feedsToolBar_->setStyleSheet("QToolBar { border: none; padding: 0px; }");
  feedsToolBar_->setIconSize(QSize(18, 18));

  QHBoxLayout *feedsPanelLayout = new QHBoxLayout();
  feedsPanelLayout->setMargin(0);
  feedsPanelLayout->setSpacing(0);
  feedsPanelLayout->addWidget(feedsTitleLabel_, 0);
  feedsPanelLayout->addStretch(1);
  feedsPanelLayout->addWidget(feedsToolBar_, 0);
  feedsPanelLayout->addSpacing(5);

  QWidget *feedsPanel = new QWidget(this);
  feedsPanel->setObjectName("feedsPanel");
  feedsPanel->setLayout(feedsPanelLayout);

  //! Create feeds DockWidget
  setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
  setDockOptions(QMainWindow::AnimatedDocks|QMainWindow::AllowNestedDocks);

  feedsDock_ = new QDockWidget(this);
  feedsDock_->setObjectName("feedsDock");
  feedsDock_->setAllowedAreas(Qt::LeftDockWidgetArea|Qt::RightDockWidgetArea|Qt::TopDockWidgetArea);
  feedsDock_->setFeatures(QDockWidget::DockWidgetMovable);
  feedsDock_->setTitleBarWidget(feedsPanel);
  feedsDock_->setWidget(feedsView_);
  addDockWidget(Qt::LeftDockWidgetArea, feedsDock_);

  connect(feedsView_, SIGNAL(pressed(QModelIndex)),
          this, SLOT(slotFeedsTreeClicked(QModelIndex)));
  connect(feedsView_, SIGNAL(pressKeyUp()), this, SLOT(slotFeedUpPressed()));
  connect(feedsView_, SIGNAL(pressKeyDown()), this, SLOT(slotFeedDownPressed()));
  connect(feedsView_, SIGNAL(customContextMenuRequested(QPoint)),
          this, SLOT(showContextMenuFeed(const QPoint &)));
  connect(feedsDock_, SIGNAL(dockLocationChanged(Qt::DockWidgetArea)),
          this, SLOT(slotFeedsDockLocationChanged(Qt::DockWidgetArea)));

  feedsView_->viewport()->installEventFilter(this);
}

void RSSListing::createNewsDock()
{
  newsView_ = new NewsView(this);
  newsView_->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  newsModel_ = new NewsModel(this, newsView_);
  newsHeader_ = new NewsHeader(Qt::Horizontal, newsView_, newsModel_);

  newsView_->setModel(newsModel_);
  newsView_->setHeader(newsHeader_);

  //! Create title DockWidget
  newsIconTitle_ = new QLabel(this);
  newsIconTitle_->setPixmap(QPixmap(":/images/feed"));
  newsTextTitle_ = new QLabel(this);
  newsTextTitle_->setObjectName("newsTextTitle_");
  QFont fontNewsTextTitle = newsTextTitle_->font();
  fontNewsTextTitle.setBold(true);
  newsTextTitle_->setFont(fontNewsTextTitle);

  QHBoxLayout *newsTitleLayout = new QHBoxLayout();
  newsTitleLayout->setMargin(4);
  newsTitleLayout->addSpacing(3);
  newsTitleLayout->addWidget(newsIconTitle_);
  newsTitleLayout->addWidget(newsTextTitle_, 1);
  newsTitleLayout->addSpacing(3);

  newsTitleLabel_ = new QWidget(this);
  newsTitleLabel_->setObjectName("newsTitleLabel_");
  newsTitleLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
  newsTitleLabel_->setLayout(newsTitleLayout);

  newsToolBar_ = new QToolBar(this);
  newsToolBar_->setStyleSheet("QToolBar { border: none; padding: 0px; }");
  newsToolBar_->setIconSize(QSize(18, 18));

  QHBoxLayout *newsPanelLayout = new QHBoxLayout();
  newsPanelLayout->setMargin(0);
  newsPanelLayout->setSpacing(0);

  newsPanelLayout->addWidget(newsTitleLabel_);
  newsPanelLayout->addStretch(1);
  newsPanelLayout->addWidget(newsToolBar_);
  newsPanelLayout->addSpacing(5);

  QWidget *newsPanel = new QWidget(this);
  newsPanel->setObjectName("newsPanel");
  newsPanel->setLayout(newsPanelLayout);

  //! Create news DockWidget
  newsDock_ = new QDockWidget(this);
  newsDock_->setObjectName("newsDock");
  newsDock_->setFeatures(QDockWidget::DockWidgetMovable);
  newsDock_->setTitleBarWidget(newsPanel);
  newsDock_->setWidget(newsView_);
  addDockWidget(Qt::TopDockWidgetArea, newsDock_);

  connect(newsView_, SIGNAL(pressed(QModelIndex)),
          this, SLOT(slotNewsViewClicked(QModelIndex)));
  connect(newsView_, SIGNAL(pressKeyUp()), this, SLOT(slotNewsUpPressed()));
  connect(newsView_, SIGNAL(pressKeyDown()), this, SLOT(slotNewsDownPressed()));
  connect(newsView_, SIGNAL(signalSetItemRead(QModelIndex, int)),
          this, SLOT(slotSetItemRead(QModelIndex, int)));
  connect(newsView_, SIGNAL(signalSetItemStar(QModelIndex,int)),
          this, SLOT(slotSetItemStar(QModelIndex,int)));
  connect(newsView_, SIGNAL(signalDoubleClicked(QModelIndex)),
          this, SLOT(slotNewsViewDoubleClicked(QModelIndex)));
  connect(newsView_, SIGNAL(customContextMenuRequested(QPoint)),
          this, SLOT(showContextMenuNews(const QPoint &)));
  connect(newsDock_, SIGNAL(dockLocationChanged(Qt::DockWidgetArea)),
          this, SLOT(slotNewsDockLocationChanged(Qt::DockWidgetArea)));
}

void RSSListing::createToolBarNull()
{
  toolBarNull_ = new QToolBar(this);
  toolBarNull_->setObjectName("toolBarNull");
  toolBarNull_->setMovable(false);
  toolBarNull_->setFixedWidth(6);
  addToolBar(Qt::LeftToolBarArea, toolBarNull_);

  pushButtonNull_ = new QPushButton(this);
  pushButtonNull_->setObjectName("pushButtonNull");
  pushButtonNull_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  pushButtonNull_->setFocusPolicy(Qt::NoFocus);
  toolBarNull_->addWidget(pushButtonNull_);
  connect(pushButtonNull_, SIGNAL(clicked()), this, SLOT(slotVisibledFeedsDock()));
  toolBarNull_->installEventFilter(this);

  connect(feedsDock_, SIGNAL(visibilityChanged(bool)),
          this, SLOT(updateIconToolBarNull(bool)));
}

void RSSListing::createWebWidget()
{
  webView_ = new WebView(this);

  webView_->pageAction(QWebPage::OpenLinkInNewWindow)->setVisible(false);
  webView_->pageAction(QWebPage::DownloadLinkToDisk)->setVisible(false);
  webView_->pageAction(QWebPage::OpenImageInNewWindow)->setVisible(false);
  webView_->pageAction(QWebPage::DownloadImageToDisk)->setVisible(false);

  webView_->setPage(new WebPage(this));

  webViewProgress_ = new QProgressBar(this);
  webViewProgress_->setObjectName("webViewProgress_");
  webViewProgress_->setFixedHeight(15);
  webViewProgress_->setMinimum(0);
  webViewProgress_->setMaximum(100);
  webViewProgress_->setVisible(true);
  connect(webView_, SIGNAL(loadStarted()), this, SLOT(slotLoadStarted()));
  connect(webView_, SIGNAL(loadFinished(bool)), this, SLOT(slotLoadFinished(bool)));
  connect(webView_, SIGNAL(linkClicked(QUrl)), this, SLOT(slotLinkClicked(QUrl)));
  connect(webView_->page(), SIGNAL(linkHovered(QString,QString,QString)),
          this, SLOT(slotLinkHovered(QString,QString,QString)));
  connect(webView_, SIGNAL(loadProgress(int)), this, SLOT(slotSetValue(int)));

  webViewProgressLabel_ = new QLabel(this);
  QHBoxLayout *progressLayout = new QHBoxLayout();
  progressLayout->setMargin(0);
  progressLayout->addWidget(webViewProgressLabel_, 0, Qt::AlignLeft|Qt::AlignVCenter);
  webViewProgress_->setLayout(progressLayout);

  //! Create web control panel
  QToolBar *webToolBar_ = new QToolBar(this);
  webToolBar_->setStyleSheet("QToolBar { border: none; padding: 0px; }");
  webToolBar_->setIconSize(QSize(16, 16));

  webHomePageAct_ = new QAction(this);
  webHomePageAct_->setIcon(QIcon(":/images/homePage"));
  connect(webHomePageAct_, SIGNAL(triggered()),
          this, SLOT(webHomePage()));

  webToolBar_->addAction(webHomePageAct_);
  QAction *webAction = webView_->pageAction(QWebPage::Back);
//  webAction->setIcon(QIcon(":/images/backPage"));
  webToolBar_->addAction(webAction);
  webAction = webView_->pageAction(QWebPage::Forward);
//  webAction->setIcon(QIcon(":/images/forwardPage"));
  webToolBar_->addAction(webAction);
  webAction = webView_->pageAction(QWebPage::Reload);
//  webAction->setIcon(QIcon(":/images/updateAllFeeds"));
  webToolBar_->addAction(webAction);
  webAction = webView_->pageAction(QWebPage::Stop);
//  webAction->setIcon(QIcon(":/images/delete"));
  webToolBar_->addAction(webAction);
  webToolBar_->addSeparator();

  webExternalBrowserAct_ = new QAction(this);
  webExternalBrowserAct_->setIcon(QIcon(":/images/openBrowser"));
  webToolBar_->addAction(webExternalBrowserAct_);
  connect(webExternalBrowserAct_, SIGNAL(triggered()),
          this, SLOT(openPageInExternalBrowser()));

  QHBoxLayout *webControlPanelHLayout = new QHBoxLayout();
  webControlPanelHLayout->setMargin(0);
  webControlPanelHLayout->addSpacing(5);
  webControlPanelHLayout->addWidget(webToolBar_);

  QFrame *webControlPanelLine = new QFrame(this);
  webControlPanelLine->setFrameStyle(QFrame::HLine | QFrame::Sunken);

  QVBoxLayout *webControlPanelLayout = new QVBoxLayout();
  webControlPanelLayout->setMargin(0);
  webControlPanelLayout->setSpacing(0);
  webControlPanelLayout->addLayout(webControlPanelHLayout);
  webControlPanelLayout->addWidget(webControlPanelLine);

  webControlPanel_ = new QWidget(this);
  webControlPanel_->setObjectName("webControlPanel_");
  webControlPanel_->setLayout(webControlPanelLayout);
  webControlPanel_->setVisible(false);

  //! Create web panel
  webPanelTitleLabel_ = new QLabel(this);
  webPanelTitleLabel_->setCursor(Qt::PointingHandCursor);
  webPanelAuthorLabel_ = new QLabel(this);

  webPanelAuthor_ = new QLabel(this);
  webPanelAuthor_->setObjectName("webPanelAuthor_");
  connect(webPanelAuthor_, SIGNAL(linkActivated(QString)),
          this, SLOT(slotWebTitleLinkClicked(QString)));

  webPanelTitle_ = new QLabel(this);
  webPanelTitle_->setObjectName("webPanelTitle_");
  connect(webPanelTitle_, SIGNAL(linkActivated(QString)),
          this, SLOT(slotWebTitleLinkClicked(QString)));

  QGridLayout *webPanelLayout1 = new QGridLayout();
  webPanelLayout1->setMargin(5);
  webPanelLayout1->setSpacing(5);
  webPanelLayout1->setColumnStretch(1, 1);
  webPanelLayout1->addWidget(webPanelTitleLabel_, 0, 0, 1, 1);
  webPanelLayout1->addWidget(webPanelTitle_, 0, 1, 1, 1);
  webPanelLayout1->addWidget(webPanelAuthorLabel_, 1, 0, 1, 1);
  webPanelLayout1->addWidget(webPanelAuthor_, 1, 1, 1, 1);

  QFrame *webPanelLine = new QFrame(this);
  webPanelLine->setFrameStyle(QFrame::HLine | QFrame::Sunken);

  QVBoxLayout *webPanelLayout = new QVBoxLayout();
  webPanelLayout->setMargin(0);
  webPanelLayout->setSpacing(0);
  webPanelLayout->addLayout(webPanelLayout1);
  webPanelLayout->addWidget(webPanelLine);
  webPanelLayout->addWidget(webControlPanel_);
//  webPanelLayout->addWidget(webPanelLine1);

  webPanel_ = new QWidget(this);
  webPanel_->setObjectName("webPanel_");
  webPanel_->setLayout(webPanelLayout);

  //! Create web layout
  QVBoxLayout *webLayout = new QVBoxLayout();
  webLayout->setMargin(0);
  webLayout->setSpacing(0);
  webLayout->addWidget(webPanel_);
  webLayout->addWidget(webView_, 1);
  webLayout->addWidget(webViewProgress_);

  webWidget_ = new QFrame(this);
  webWidget_->setObjectName("webWidget_");
  webWidget_->setLayout(webLayout);
  webWidget_->setMinimumWidth(400);
  webWidget_->setFrameStyle(QFrame::Panel | QFrame::Sunken);

  setCentralWidget(webWidget_);
}

void RSSListing::createStatusBar()
{
  progressBar_ = new QProgressBar(this);
  progressBar_->setObjectName("progressBar_");
  progressBar_->setFixedWidth(100);
  progressBar_->setFixedHeight(14);
  progressBar_->setMinimum(0);
  progressBar_->setMaximum(0);
  progressBar_->setTextVisible(false);
  progressBar_->setVisible(false);
  statusBar()->setMinimumHeight(22);
  statusBar()->addPermanentWidget(progressBar_);
  statusUnread_ = new QLabel(this);
  statusBar()->addPermanentWidget(statusUnread_);
  statusAll_ = new QLabel(this);
  statusBar()->addPermanentWidget(statusAll_);
  statusBar()->setVisible(true);
}

void RSSListing::createTray()
{
  traySystem = new QSystemTrayIcon(QIcon(":/images/quiterss16"), this);
  connect(traySystem,SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
          this, SLOT(slotActivationTray(QSystemTrayIcon::ActivationReason)));
  connect(this, SIGNAL(signalPlaceToTray()),
          this, SLOT(slotPlaceToTray()), Qt::QueuedConnection);
  traySystem->setToolTip("QuiteRSS");
  createTrayMenu();
}

/*! \brief Создание действий **************************************************
 * \details Которые будут использоваться в главном меню и ToolBar
 ******************************************************************************/
void RSSListing::createActions()
{
  addFeedAct_ = new QAction(this);
  addFeedAct_->setObjectName("addFeedAct");
  addFeedAct_->setIcon(QIcon(":/images/add"));
  connect(addFeedAct_, SIGNAL(triggered()), this, SLOT(addFeed()));

  deleteFeedAct_ = new QAction(this);
  deleteFeedAct_->setObjectName("deleteFeedAct");
  deleteFeedAct_->setIcon(QIcon(":/images/delete"));
  connect(deleteFeedAct_, SIGNAL(triggered()), this, SLOT(deleteFeed()));

  importFeedsAct_ = new QAction(this);
  importFeedsAct_->setObjectName("importFeedsAct");
  importFeedsAct_->setIcon(QIcon(":/images/importFeeds"));
  connect(importFeedsAct_, SIGNAL(triggered()), this, SLOT(slotImportFeeds()));

  exportFeedsAct_ = new QAction(this);
  exportFeedsAct_->setObjectName("exportFeedsAct");
  exportFeedsAct_->setIcon(QIcon(":/images/exportFeeds"));
  connect(exportFeedsAct_, SIGNAL(triggered()), this, SLOT(slotExportFeeds()));

  exitAct_ = new QAction(this);
  exitAct_->setObjectName("exitAct");
  connect(exitAct_, SIGNAL(triggered()), this, SLOT(slotClose()));

  toolBarToggle_ = new QAction(this);
  toolBarToggle_->setCheckable(true);
  toolBarStyleI_ = new QAction(this);
  toolBarStyleI_->setObjectName("toolBarStyleI_");
  toolBarStyleI_->setCheckable(true);
  toolBarStyleT_ = new QAction(this);
  toolBarStyleT_->setObjectName("toolBarStyleT_");
  toolBarStyleT_->setCheckable(true);
  toolBarStyleTbI_ = new QAction(this);
  toolBarStyleTbI_->setObjectName("toolBarStyleTbI_");
  toolBarStyleTbI_->setCheckable(true);
  toolBarStyleTuI_ = new QAction(this);
  toolBarStyleTuI_->setObjectName("toolBarStyleTuI_");
  toolBarStyleTuI_->setCheckable(true);
  toolBarStyleTuI_->setChecked(true);

  toolBarIconBig_ = new QAction(this);
  toolBarIconBig_->setObjectName("toolBarIconBig_");
  toolBarIconBig_->setCheckable(true);
  toolBarIconNormal_ = new QAction(this);
  toolBarIconNormal_->setObjectName("toolBarIconNormal_");
  toolBarIconNormal_->setCheckable(true);
  toolBarIconNormal_->setChecked(true);
  toolBarIconSmall_ = new QAction(this);
  toolBarIconSmall_->setObjectName("toolBarIconSmall_");
  toolBarIconSmall_->setCheckable(true);

  systemStyle_ = new QAction(this);
  systemStyle_->setObjectName("systemStyle_");
  systemStyle_->setCheckable(true);
  system2Style_ = new QAction(this);
  system2Style_->setObjectName("system2Style_");
  system2Style_->setCheckable(true);
  greenStyle_ = new QAction(this);
  greenStyle_->setObjectName("greenStyle_");
  greenStyle_->setCheckable(true);
  greenStyle_->setChecked(true);
  orangeStyle_ = new QAction(this);
  orangeStyle_->setObjectName("orangeStyle_");
  orangeStyle_->setCheckable(true);
  purpleStyle_ = new QAction(this);
  purpleStyle_->setObjectName("purpleStyle_");
  purpleStyle_->setCheckable(true);
  pinkStyle_ = new QAction(this);
  pinkStyle_->setObjectName("pinkStyle_");
  pinkStyle_->setCheckable(true);
  grayStyle_ = new QAction(this);
  grayStyle_->setObjectName("grayStyle_");
  grayStyle_->setCheckable(true);

  autoLoadImagesToggle_ = new QAction(this);
  autoLoadImagesToggle_->setObjectName("autoLoadImagesToggle");

  updateFeedAct_ = new QAction(this);
  updateFeedAct_->setObjectName("updateFeedAct");
  updateFeedAct_->setIcon(QIcon(":/images/updateFeed"));
  connect(updateFeedAct_, SIGNAL(triggered()), this, SLOT(slotGetFeed()));
  connect(feedsView_, SIGNAL(doubleClicked(QModelIndex)),
          updateFeedAct_, SLOT(trigger()));

  updateAllFeedsAct_ = new QAction(this);
  updateAllFeedsAct_->setObjectName("updateAllFeedsAct");
  updateAllFeedsAct_->setIcon(QIcon(":/images/updateAllFeeds"));
  connect(updateAllFeedsAct_, SIGNAL(triggered()), this, SLOT(slotGetAllFeeds()));

  markAllFeedRead_ = new QAction(this);
  markAllFeedRead_->setObjectName("markAllFeedRead");
  markAllFeedRead_->setIcon(QIcon(":/images/markReadAll"));
  connect(markAllFeedRead_, SIGNAL(triggered()), this, SLOT(markAllFeedsRead()));

  markNewsRead_ = new QAction(this);
  markNewsRead_->setObjectName("markNewsRead");
  markNewsRead_->setIcon(QIcon(":/images/markRead"));
  connect(markNewsRead_, SIGNAL(triggered()), this, SLOT(markNewsRead()));

  markAllNewsRead_ = new QAction(this);
  markAllNewsRead_->setObjectName("markAllNewsRead");
  markAllNewsRead_->setIcon(QIcon(":/images/markReadAll"));
  connect(markAllNewsRead_, SIGNAL(triggered()), this, SLOT(markAllNewsRead()));

  setNewsFiltersAct_ = new QAction(this);
  setNewsFiltersAct_->setIcon(QIcon(":/images/filterOff"));
  connect(setNewsFiltersAct_, SIGNAL(triggered()), this, SLOT(showNewsFiltersDlg()));
  setFilterNewsAct_ = new QAction(this);
  setFilterNewsAct_->setIcon(QIcon(":/images/filterOff"));
  connect(setFilterNewsAct_, SIGNAL(triggered()), this, SLOT(showFilterNewsDlg()));

  optionsAct_ = new QAction(this);
  optionsAct_->setObjectName("optionsAct");
  optionsAct_->setIcon(QIcon(":/images/options"));
  connect(optionsAct_, SIGNAL(triggered()), this, SLOT(showOptionDlg()));

  feedsFilter_ = new QAction(this);
  feedsFilter_->setIcon(QIcon(":/images/filterOff"));
  filterFeedsAll_ = new QAction(this);
  filterFeedsAll_->setObjectName("filterFeedsAll_");
  filterFeedsAll_->setCheckable(true);
  filterFeedsAll_->setChecked(true);
  filterFeedsNew_ = new QAction(this);
  filterFeedsNew_->setObjectName("filterFeedsNew_");
  filterFeedsNew_->setCheckable(true);
  filterFeedsUnread_ = new QAction(this);
  filterFeedsUnread_->setObjectName("filterFeedsUnread_");
  filterFeedsUnread_->setCheckable(true);

  newsFilter_ = new QAction(this);
  newsFilter_->setIcon(QIcon(":/images/filterOff"));
  filterNewsAll_ = new QAction(this);
  filterNewsAll_->setObjectName("filterNewsAll_");
  filterNewsAll_->setCheckable(true);
  filterNewsAll_->setChecked(true);
  filterNewsNew_ = new QAction(this);
  filterNewsNew_->setObjectName("filterNewsNew_");
  filterNewsNew_->setCheckable(true);
  filterNewsUnread_ = new QAction( this);
  filterNewsUnread_->setObjectName("filterNewsUnread_");
  filterNewsUnread_->setCheckable(true);
  filterNewsStar_ = new QAction(this);
  filterNewsStar_->setObjectName("filterNewsStar_");
  filterNewsStar_->setCheckable(true);

  aboutAct_ = new QAction(this);
  aboutAct_->setObjectName("AboutAct_");
  connect(aboutAct_, SIGNAL(triggered()), this, SLOT(slotShowAboutDlg()));

  updateAppAct_ = new QAction(this);
  updateAppAct_->setObjectName("UpdateApp_");
  connect(updateAppAct_, SIGNAL(triggered()), this, SLOT(slotShowUpdateAppDlg()));

  openNewsWebViewAct_ = new QAction(this);
  openNewsWebViewAct_->setShortcut(QKeySequence(Qt::Key_Return));
  this->addAction(openNewsWebViewAct_);
  connect(openNewsWebViewAct_, SIGNAL(triggered()), this, SLOT(slotOpenNewsWebView()));
  openInBrowserAct_ = new QAction(this);
  openInBrowserAct_->setObjectName("openInBrowserAct");
  this->addAction(openInBrowserAct_);
  connect(openInBrowserAct_, SIGNAL(triggered()), this, SLOT(openInBrowserNews()));
  openInExternalBrowserAct_ = new QAction(this);
  openInExternalBrowserAct_->setObjectName("openInExternalBrowserAct");
  this->addAction(openInExternalBrowserAct_);
  connect(openInExternalBrowserAct_, SIGNAL(triggered()),
          this, SLOT(openInExternalBrowserNews()));

  markStarAct_ = new QAction(this);
  markStarAct_->setObjectName("markStarAct");
  markStarAct_->setIcon(QIcon(":/images/starOn"));
  connect(markStarAct_, SIGNAL(triggered()), this, SLOT(markNewsStar()));
  deleteNewsAct_ = new QAction(this);
  deleteNewsAct_->setObjectName("deleteNewsAct");
  deleteNewsAct_->setIcon(QIcon(":/images/delete"));
  connect(deleteNewsAct_, SIGNAL(triggered()), this, SLOT(deleteNews()));
  this->addAction(deleteNewsAct_);

  markFeedRead_ = new QAction(this);
  markFeedRead_->setObjectName("markFeedRead");
  markFeedRead_->setIcon(QIcon(":/images/markRead"));
  connect(markFeedRead_, SIGNAL(triggered()), this, SLOT(markAllNewsRead()));
  feedProperties_ = new QAction(this);
  feedProperties_->setObjectName("feedProperties");
  feedProperties_->setIcon(QIcon(":/images/preferencesFeed"));
  connect(feedProperties_, SIGNAL(triggered()), this, SLOT(slotShowFeedPropertiesDlg()));

  feedKeyUpAct_ = new QAction(this);
  feedKeyUpAct_->setObjectName("feedKeyUp");
  connect(feedKeyUpAct_, SIGNAL(triggered()), this, SLOT(slotFeedUpPressed()));
  this->addAction(feedKeyUpAct_);

  feedKeyDownAct_ = new QAction(this);
  feedKeyDownAct_->setObjectName("feedKeyDownAct");
  connect(feedKeyDownAct_, SIGNAL(triggered()), this, SLOT(slotFeedDownPressed()));
  this->addAction(feedKeyDownAct_);

  newsKeyUpAct_ = new QAction(this);
  newsKeyUpAct_->setObjectName("newsKeyUpAct");
  connect(newsKeyUpAct_, SIGNAL(triggered()), this, SLOT(slotNewsUpPressed()));
  this->addAction(newsKeyUpAct_);

  newsKeyDownAct_ = new QAction(this);
  newsKeyDownAct_->setObjectName("newsKeyDownAct");
  connect(newsKeyDownAct_, SIGNAL(triggered()), this, SLOT(slotNewsDownPressed()));
  this->addAction(newsKeyDownAct_);

  switchFocusAct_ = new QAction(this);
  switchFocusAct_->setObjectName("switchFocusAct");
  connect(switchFocusAct_, SIGNAL(triggered()), this, SLOT(slotSwitchFocus()));
  this->addAction(switchFocusAct_);

  visibleFeedsDockAct_ = new QAction(this);
  visibleFeedsDockAct_->setObjectName("visibleFeedsDockAct");
  connect(visibleFeedsDockAct_, SIGNAL(triggered()), this, SLOT(slotVisibledFeedsDock()));
  this->addAction(visibleFeedsDockAct_);
}

void RSSListing::createShortcut()
{
  addFeedAct_->setShortcut(QKeySequence(QKeySequence::New));
  listActions_.append(addFeedAct_);
  deleteFeedAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Delete));
  listActions_.append(deleteFeedAct_);
  exitAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Q));  // standart on other OS
  listActions_.append(exitAct_);
  updateFeedAct_->setShortcut(QKeySequence(Qt::Key_F5));
  listActions_.append(updateFeedAct_);
  updateAllFeedsAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_F5));
  listActions_.append(updateAllFeedsAct_);
  optionsAct_->setShortcut(QKeySequence(Qt::Key_F8));
  listActions_.append(optionsAct_);
  deleteNewsAct_->setShortcut(QKeySequence(Qt::Key_Delete));
  listActions_.append(deleteNewsAct_);
  feedProperties_->setShortcut(QKeySequence(Qt::CTRL+Qt::Key_E));
  listActions_.append(feedProperties_);
  feedKeyUpAct_->setShortcut(QKeySequence(Qt::CTRL+Qt::Key_Up));
  listActions_.append(feedKeyUpAct_);
  feedKeyDownAct_->setShortcut(QKeySequence(Qt::CTRL+Qt::Key_Down));
  listActions_.append(feedKeyDownAct_);
  newsKeyUpAct_->setShortcut(QKeySequence(Qt::Key_Left));
  listActions_.append(newsKeyUpAct_);
  newsKeyDownAct_->setShortcut(QKeySequence(Qt::Key_Right));
  listActions_.append(newsKeyDownAct_);

  listActions_.append(importFeedsAct_);
  listActions_.append(exportFeedsAct_);
  listActions_.append(autoLoadImagesToggle_);
  listActions_.append(markAllFeedRead_);
  listActions_.append(markFeedRead_);
  listActions_.append(markNewsRead_);
  listActions_.append(markAllNewsRead_);
  listActions_.append(markStarAct_);

  listActions_.append(openInBrowserAct_);
  openInBrowserAct_->setShortcut(QKeySequence(Qt::Key_O));
  listActions_.append(openInExternalBrowserAct_);
  openInExternalBrowserAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_O));

  switchFocusAct_->setShortcut(QKeySequence(Qt::CTRL+Qt::Key_Tab));
  listActions_.append(switchFocusAct_);

  visibleFeedsDockAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_F));
  listActions_.append(visibleFeedsDockAct_);

  loadActionShortcuts();
}

void RSSListing::loadActionShortcuts()
{
  settings_->beginGroup("/Shortcuts");

  QListIterator<QAction *> iter(listActions_);
  while (iter.hasNext()) {
    QAction *pAction = iter.next();
    if (pAction->objectName().isEmpty())
      continue;

    listDefaultShortcut_.append(pAction->shortcut());

    const QString& sKey = '/' + pAction->objectName();
    const QString& sValue = settings_->value('/' + sKey).toString();
    if (sValue.isEmpty())
      continue;
    pAction->setShortcut(QKeySequence(sValue));
  }

  settings_->endGroup();
}

void RSSListing::saveActionShortcuts()
{
  settings_->beginGroup("/Shortcuts/");

  QListIterator<QAction *> iter(listActions_);
  while (iter.hasNext()) {
    QAction *pAction = iter.next();
    if (pAction->objectName().isEmpty())
      continue;

    const QString& sKey = '/' + pAction->objectName();
    const QString& sValue = QString(pAction->shortcut());
    if (!sValue.isEmpty())
      settings_->setValue(sKey, sValue);
    else if (settings_->contains(sKey))
      settings_->remove(sKey);
  }

  settings_->endGroup();
}

/*! \brief Создание главного меню *********************************************/
void RSSListing::createMenu()
{
  fileMenu_ = new QMenu(this);
  menuBar()->addMenu(fileMenu_);
  fileMenu_->addAction(addFeedAct_);
  fileMenu_->addAction(deleteFeedAct_);
  fileMenu_->addSeparator();
  fileMenu_->addAction(importFeedsAct_);
  fileMenu_->addAction(exportFeedsAct_);
  fileMenu_->addSeparator();
  fileMenu_->addAction(exitAct_);

  editMenu_ = new QMenu(this);
  connect(editMenu_, SIGNAL(aboutToShow()), this, SLOT(slotEditMenuAction()));
  menuBar()->addMenu(editMenu_);
  editMenu_->addAction(feedProperties_);

  viewMenu_  = new QMenu(this);
  menuBar()->addMenu(viewMenu_ );

  toolBarMenu_ = new QMenu(this);
  toolBarStyleMenu_ = new QMenu(this);
  toolBarStyleMenu_->addAction(toolBarStyleI_);
  toolBarStyleMenu_->addAction(toolBarStyleT_);
  toolBarStyleMenu_->addAction(toolBarStyleTbI_);
  toolBarStyleMenu_->addAction(toolBarStyleTuI_);
  toolBarStyleGroup_ = new QActionGroup(this);
  toolBarStyleGroup_->addAction(toolBarStyleI_);
  toolBarStyleGroup_->addAction(toolBarStyleT_);
  toolBarStyleGroup_->addAction(toolBarStyleTbI_);
  toolBarStyleGroup_->addAction(toolBarStyleTuI_);
  connect(toolBarStyleGroup_, SIGNAL(triggered(QAction*)),
          this, SLOT(setToolBarStyle(QAction*)));

  toolBarIconSizeMenu_ = new QMenu(this);
  toolBarIconSizeMenu_->addAction(toolBarIconBig_);
  toolBarIconSizeMenu_->addAction(toolBarIconNormal_);
  toolBarIconSizeMenu_->addAction(toolBarIconSmall_);
  toolBarIconSizeGroup_ = new QActionGroup(this);
  toolBarIconSizeGroup_->addAction(toolBarIconBig_);
  toolBarIconSizeGroup_->addAction(toolBarIconNormal_);
  toolBarIconSizeGroup_->addAction(toolBarIconSmall_);
  connect(toolBarIconSizeGroup_, SIGNAL(triggered(QAction*)),
          this, SLOT(setToolBarIconSize(QAction*)));

  toolBarMenu_->addMenu(toolBarStyleMenu_);
  toolBarMenu_->addMenu(toolBarIconSizeMenu_);
  toolBarMenu_->addAction(toolBarToggle_);
  viewMenu_->addMenu(toolBarMenu_);

  styleMenu_ = new QMenu(this);
  styleMenu_->addAction(systemStyle_);
  styleMenu_->addAction(system2Style_);
  styleMenu_->addAction(greenStyle_);
  styleMenu_->addAction(orangeStyle_);
  styleMenu_->addAction(purpleStyle_);
  styleMenu_->addAction(pinkStyle_);
  styleMenu_->addAction(grayStyle_);
  styleGroup_ = new QActionGroup(this);
  styleGroup_->addAction(systemStyle_);
  styleGroup_->addAction(system2Style_);
  styleGroup_->addAction(greenStyle_);
  styleGroup_->addAction(orangeStyle_);
  styleGroup_->addAction(purpleStyle_);
  styleGroup_->addAction(pinkStyle_);
  styleGroup_->addAction(grayStyle_);
  connect(styleGroup_, SIGNAL(triggered(QAction*)),
          this, SLOT(setStyleApp(QAction*)));
  viewMenu_->addMenu(styleMenu_);

  feedMenu_ = new QMenu(this);
  menuBar()->addMenu(feedMenu_);
  feedMenu_->addAction(updateFeedAct_);
  feedMenu_->addAction(updateAllFeedsAct_);
  feedMenu_->addSeparator();
  feedMenu_->addAction(markAllFeedRead_);
  feedMenu_->addSeparator();

  feedsFilterGroup_ = new QActionGroup(this);
  feedsFilterGroup_->setExclusive(true);
  connect(feedsFilterGroup_, SIGNAL(triggered(QAction*)),
          this, SLOT(setFeedsFilter(QAction*)));

  feedsFilterMenu_ = new QMenu(this);
  feedsFilterMenu_->addAction(filterFeedsAll_);
  feedsFilterGroup_->addAction(filterFeedsAll_);
  feedsFilterMenu_->addSeparator();
  feedsFilterMenu_->addAction(filterFeedsNew_);
  feedsFilterGroup_->addAction(filterFeedsNew_);
  feedsFilterMenu_->addAction(filterFeedsUnread_);
  feedsFilterGroup_->addAction(filterFeedsUnread_);

  feedsFilter_->setMenu(feedsFilterMenu_);
  feedMenu_->addAction(feedsFilter_);
  feedsToolBar_->addAction(feedsFilter_);
  feedsFilterAction = NULL;
  connect(feedsFilter_, SIGNAL(triggered()), this, SLOT(slotFeedsFilter()));

  newsMenu_ = new QMenu(this);
  menuBar()->addMenu(newsMenu_);
  newsMenu_->addAction(markNewsRead_);
  newsMenu_->addAction(markAllNewsRead_);
  newsMenu_->addSeparator();
  newsMenu_->addAction(markStarAct_);
  newsMenu_->addSeparator();

  newsFilterGroup_ = new QActionGroup(this);
  newsFilterGroup_->setExclusive(true);
  connect(newsFilterGroup_, SIGNAL(triggered(QAction*)),
          this, SLOT(setNewsFilter(QAction*)));

  newsFilterMenu_ = new QMenu(this);
  newsFilterMenu_->addAction(filterNewsAll_);
  newsFilterGroup_->addAction(filterNewsAll_);
  newsFilterMenu_->addSeparator();
  newsFilterMenu_->addAction(filterNewsNew_);
  newsFilterGroup_->addAction(filterNewsNew_);
  newsFilterMenu_->addAction(filterNewsUnread_);
  newsFilterGroup_->addAction(filterNewsUnread_);
  newsFilterMenu_->addAction(filterNewsStar_);
  newsFilterGroup_->addAction(filterNewsStar_);

  newsFilter_->setMenu(newsFilterMenu_);
  newsMenu_->addAction(newsFilter_);
  newsToolBar_->addAction(newsFilter_);
  newsFilterAction = NULL;
  connect(newsFilter_, SIGNAL(triggered()), this, SLOT(slotNewsFilter()));

  newsMenu_->addSeparator();
  newsMenu_->addAction(autoLoadImagesToggle_);

  toolsMenu_ = new QMenu(this);
  menuBar()->addMenu(toolsMenu_);
  //  toolsMenu_->addAction(setNewsFiltersAct_);
  toolsMenu_->addSeparator();
  toolsMenu_->addAction(optionsAct_);

  helpMenu_ = new QMenu(this);
  menuBar()->addMenu(helpMenu_);
  helpMenu_->addAction(updateAppAct_);
  helpMenu_->addSeparator();
  helpMenu_->addAction(aboutAct_);
}

/*! \brief Создание ToolBar ***************************************************/
void RSSListing::createToolBar()
{
  toolBar_ = new QToolBar(this);
  addToolBar(toolBar_);
  toolBar_->setObjectName("ToolBar_General");
  toolBar_->setAllowedAreas(Qt::TopToolBarArea);
  toolBar_->setMovable(false);
  toolBar_->setContextMenuPolicy(Qt::CustomContextMenu);
  toolBar_->addAction(addFeedAct_);
  //  toolBar_->addAction(deleteFeedAct_);
  toolBar_->addSeparator();
  toolBar_->addAction(updateFeedAct_);
  toolBar_->addAction(updateAllFeedsAct_);
  toolBar_->addSeparator();
  toolBar_->addAction(markNewsRead_);
  toolBar_->addAction(markAllNewsRead_);
  toolBar_->addSeparator();
  toolBar_->addAction(autoLoadImagesToggle_);
  toolBar_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  connect(toolBar_, SIGNAL(visibilityChanged(bool)),
          toolBarToggle_, SLOT(setChecked(bool)));
  connect(toolBarToggle_, SIGNAL(toggled(bool)),
          toolBar_, SLOT(setVisible(bool)));
  connect(autoLoadImagesToggle_, SIGNAL(triggered()),
          this, SLOT(setAutoLoadImages()));
  connect(toolBar_, SIGNAL(customContextMenuRequested(QPoint)),
          this, SLOT(showContextMenuToolBar(const QPoint &)));
}

/*! \brief Чтение настроек из ini-файла ***************************************/
void RSSListing::readSettings()
{
  settings_->beginGroup("/Settings");

  showSplashScreen_ = settings_->value("showSplashScreen", true).toBool();
  reopenFeedStartup_ = settings_->value("reopenFeedStartup", true).toBool();

  showTrayIcon_ = settings_->value("showTrayIcon", true).toBool();
  startingTray_ = settings_->value("startingTray", false).toBool();
  minimizingTray_ = settings_->value("minimizingTray", true).toBool();
  closingTray_ = settings_->value("closingTray", false).toBool();
  behaviorIconTray_ = settings_->value("behaviorIconTray", 2).toInt();
  singleClickTray_ = settings_->value("singleClickTray", false).toBool();
  clearStatusNew_ = settings_->value("clearStatusNew", true).toBool();
  emptyWorking_ = settings_->value("emptyWorking", true).toBool();

  QString strLang("en");
  QString strLocalLang = QLocale::system().name().left(2);
  QDir langDir = qApp->applicationDirPath() + "/lang";
  foreach (QString file, langDir.entryList(QStringList("*.qm"), QDir::Files)) {
    if (strLocalLang == file.section('.', 0, 0).section('_', 1))
      strLang = strLocalLang;
  }
  langFileName_ = settings_->value("langFileName", strLang).toString();

  QString fontFamily = settings_->value("/feedsFontFamily", qApp->font().family()).toString();
  int fontSize = settings_->value("/feedsFontSize", 8).toInt();
  feedsView_->setFont(QFont(fontFamily, fontSize));
  feedsModel_->font_ = feedsView_->font();

  fontFamily = settings_->value("/newsFontFamily", qApp->font().family()).toString();
  fontSize = settings_->value("/newsFontSize", 8).toInt();
  newsView_->setFont(QFont(fontFamily, fontSize));
  fontFamily = settings_->value("/WebFontFamily", qApp->font().family()).toString();
  fontSize = settings_->value("/WebFontSize", 12).toInt();
  webView_->settings()->setFontFamily(QWebSettings::StandardFont, fontFamily);
  webView_->settings()->setFontSize(QWebSettings::DefaultFontSize, fontSize);

  autoUpdatefeedsStartUp_ = settings_->value("autoUpdatefeedsStartUp", false).toBool();
  autoUpdatefeeds_ = settings_->value("autoUpdatefeeds", false).toBool();
  autoUpdatefeedsTime_ = settings_->value("autoUpdatefeedsTime", 10).toInt();
  autoUpdatefeedsInterval_ = settings_->value("autoUpdatefeedsInterval", 0).toInt();

  openingFeedAction_ = settings_->value("openingFeedAction", 0).toInt();
  openNewsWebViewOn_ = settings_->value("openNewsWebViewOn", true).toBool();

  markNewsReadOn_ = settings_->value("markNewsReadOn", true).toBool();
  markNewsReadTime_ = settings_->value("markNewsReadTime", 0).toInt();

  showDescriptionNews_ = settings_->value("showDescriptionNews", true).toBool();

  maxDayCleanUp_ = settings_->value("maxDayClearUp", 30).toInt();
  maxNewsCleanUp_ = settings_->value("maxNewsClearUp", 200).toInt();
  dayCleanUpOn_ = settings_->value("dayClearUpOn", true).toBool();
  newsCleanUpOn_ = settings_->value("newsClearUpOn", true).toBool();
  readCleanUp_ = settings_->value("readClearUp", false).toBool();
  neverUnreadCleanUp_ = settings_->value("neverUnreadClearUp", true).toBool();
  neverStarCleanUp_ = settings_->value("neverStarClearUp", true).toBool();

  embeddedBrowserOn_ = settings_->value("embeddedBrowserOn", false).toBool();
  if (embeddedBrowserOn_) {
    webView_->page()->setLinkDelegationPolicy(QWebPage::DelegateAllLinks);
    openInExternalBrowserAct_->setVisible(true);
  } else {
    webView_->page()->setLinkDelegationPolicy(QWebPage::DelegateExternalLinks);
    openInExternalBrowserAct_->setVisible(false);
  }
  javaScriptEnable_ = settings_->value("javaScriptEnable", true).toBool();
  webView_->settings()->setAttribute(
        QWebSettings::JavascriptEnabled, javaScriptEnable_);
  pluginsEnable_ = settings_->value("pluginsEnable", true).toBool();
  webView_->settings()->setAttribute(
        QWebSettings::PluginsEnabled, pluginsEnable_);


  soundNewNews_ = settings_->value("soundNewNews", true).toBool();

  QString str = settings_->value("toolBarStyle", "toolBarStyleTuI_").toString();
  QList<QAction*> listActions = toolBarStyleGroup_->actions();
  foreach(QAction *action, listActions) {
    if (action->objectName() == str) {
      action->setChecked(true);
      break;
    }
  }
  setToolBarStyle(toolBarStyleGroup_->checkedAction());
  str = settings_->value("toolBarIconSize", "toolBarIconNormal_").toString();
  listActions = toolBarIconSizeGroup_->actions();
  foreach(QAction *action, listActions) {
    if (action->objectName() == str) {
      action->setChecked(true);
      break;
    }
  }
  setToolBarIconSize(toolBarIconSizeGroup_->checkedAction());

  str = settings_->value("styleApplication", "defaultStyle_").toString();
  listActions = styleGroup_->actions();
  foreach(QAction *action, listActions) {
    if (action->objectName() == str) {
      action->setChecked(true);
      break;
    }
  }

  settings_->endGroup();

  resize(800, 600);
  restoreGeometry(settings_->value("GeometryState").toByteArray());
  restoreState(settings_->value("ToolBarsState").toByteArray());

  toolBarNull_->setStyleSheet("QToolBar { border: none; padding: 0px;}");
  if (feedsDockArea_ == Qt::LeftDockWidgetArea) {
      pushButtonNull_->setIcon(QIcon(":/images/images/triangleL.png"));
  } else if (feedsDockArea_ == Qt::RightDockWidgetArea) {
      pushButtonNull_->setIcon(QIcon(":/images/images/triangleR.png"));
  } else {
    pushButtonNull_->setIcon(QIcon(":/images/images/triangleR.png"));
  }

  networkProxy_.setType(static_cast<QNetworkProxy::ProxyType>(
                          settings_->value("networkProxy/type", QNetworkProxy::DefaultProxy).toInt()));
  networkProxy_.setHostName(settings_->value("networkProxy/hostName", "").toString());
  networkProxy_.setPort(    settings_->value("networkProxy/port",     "").toUInt());
  networkProxy_.setUser(    settings_->value("networkProxy/user",     "").toString());
  networkProxy_.setPassword(settings_->value("networkProxy/password", "").toString());
  persistentUpdateThread_->setProxy(networkProxy_);
}

/*! \brief Запись настроек в ini-файл *****************************************/
void RSSListing::writeSettings()
{
  settings_->beginGroup("/Settings");

  settings_->setValue("showSplashScreen", showSplashScreen_);
  settings_->setValue("reopenFeedStartup", reopenFeedStartup_);

  settings_->setValue("showTrayIcon", showTrayIcon_);
  settings_->setValue("startingTray", startingTray_);
  settings_->setValue("minimizingTray", minimizingTray_);
  settings_->setValue("closingTray", closingTray_);
  settings_->setValue("behaviorIconTray", behaviorIconTray_);
  settings_->setValue("singleClickTray", singleClickTray_);
  settings_->setValue("clearStatusNew", clearStatusNew_);
  settings_->setValue("emptyWorking", emptyWorking_);

  settings_->setValue("langFileName", langFileName_);

  QString fontFamily = feedsView_->font().family();
  settings_->setValue("/feedsFontFamily", fontFamily);
  int fontSize = feedsView_->font().pointSize();
  settings_->setValue("/feedsFontSize", fontSize);

  fontFamily = newsView_->font().family();
  settings_->setValue("/newsFontFamily", fontFamily);
  fontSize = newsView_->font().pointSize();
  settings_->setValue("/newsFontSize", fontSize);

  fontFamily = webView_->settings()->fontFamily(QWebSettings::StandardFont);
  settings_->setValue("/WebFontFamily", fontFamily);
  fontSize = webView_->settings()->fontSize(QWebSettings::DefaultFontSize);
  settings_->setValue("/WebFontSize", fontSize);

  settings_->setValue("autoLoadImages", autoLoadImages_);
  settings_->setValue("autoUpdatefeedsStartUp", autoUpdatefeedsStartUp_);
  settings_->setValue("autoUpdatefeeds", autoUpdatefeeds_);
  settings_->setValue("autoUpdatefeedsTime", autoUpdatefeedsTime_);
  settings_->setValue("autoUpdatefeedsInterval", autoUpdatefeedsInterval_);

  settings_->setValue("openingFeedAction", openingFeedAction_);
  settings_->setValue("openNewsWebViewOn", openNewsWebViewOn_);

  settings_->setValue("markNewsReadOn", markNewsReadOn_);
  settings_->setValue("markNewsReadTime", markNewsReadTime_);

  settings_->setValue("showDescriptionNews", showDescriptionNews_);

  settings_->setValue("maxDayClearUp", maxDayCleanUp_);
  settings_->setValue("maxNewsClearUp", maxNewsCleanUp_);
  settings_->setValue("dayClearUpOn", dayCleanUpOn_);
  settings_->setValue("newsClearUpOn", newsCleanUpOn_);
  settings_->setValue("readClearUp", readCleanUp_);
  settings_->setValue("neverUnreadClearUp", neverUnreadCleanUp_);
  settings_->setValue("neverStarClearUp", neverStarCleanUp_);

  settings_->setValue("embeddedBrowserOn", embeddedBrowserOn_);
  settings_->setValue("javaScriptEnable", javaScriptEnable_);
  settings_->setValue("pluginsEnable", pluginsEnable_);

  settings_->setValue("soundNewNews", soundNewNews_);

  settings_->setValue("toolBarStyle",
                      toolBarStyleGroup_->checkedAction()->objectName());
  settings_->setValue("toolBarIconSize",
                      toolBarIconSizeGroup_->checkedAction()->objectName());

  settings_->setValue("styleApplication",
                      styleGroup_->checkedAction()->objectName());

  settings_->endGroup();

  settings_->setValue("GeometryState", saveGeometry());
  settings_->setValue("ToolBarsState", saveState());
  if (newsModel_->columnCount()) {
    settings_->setValue("NewsHeaderGeometry", newsHeader_->saveGeometry());
    settings_->setValue("NewsHeaderState", newsHeader_->saveState());
  }

  settings_->setValue("networkProxy/type",     networkProxy_.type());
  settings_->setValue("networkProxy/hostName", networkProxy_.hostName());
  settings_->setValue("networkProxy/port",     networkProxy_.port());
  settings_->setValue("networkProxy/user",     networkProxy_.user());
  settings_->setValue("networkProxy/password", networkProxy_.password());

  settings_->setValue("feedSettings/currentId",
                      feedsModel_->index(feedsView_->currentIndex().row(), 0).data().toInt());
  settings_->setValue("feedSettings/filterName",
                      feedsFilterGroup_->checkedAction()->objectName());
  settings_->setValue("newsSettings/filterName",
                      newsFilterGroup_->checkedAction()->objectName());
}

/*! \brief Добавление ленты в список лент *************************************/
void RSSListing::addFeed()
{
  AddFeedWizard *addFeedWizard = new AddFeedWizard(this, &db_);

  if (addFeedWizard->exec() == QDialog::Rejected) {
    delete addFeedWizard;
    return;
  }

  emit startGetUrlTimer();
  faviconLoader->requestUrl(addFeedWizard->htmlUrlString_,
                            addFeedWizard->feedUrlString_);
  slotUpdateFeed(addFeedWizard->feedUrlString_, true);

  delete addFeedWizard;
}

/*! \brief Удаление ленты из списка лент с подтверждением *********************/
void RSSListing::deleteFeed()
{
  if (feedsView_->currentIndex().isValid()) {
    QMessageBox msgBox;
    msgBox.setIcon(QMessageBox::Question);
    msgBox.setWindowTitle(tr("Delete feed"));
    msgBox.setText(QString(tr("Are you sure to delete the feed '%1'?")).
                   arg(feedsModel_->record(feedsView_->currentIndex().row()).field("text").value().toString()));
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msgBox.setDefaultButton(QMessageBox::No);

    if (msgBox.exec() == QMessageBox::No) return;

    int id = feedsModel_->record(
          feedsView_->currentIndex().row()).field("id").value().toInt();
    db_.transaction();
    QSqlQuery q(db_);
    q.exec(QString("DELETE FROM feeds WHERE id='%1'").arg(id));
    q.exec(QString("DELETE FROM news WHERE feedId='%1'").arg(id));
    q.exec("VACUUM");
    q.finish();
    db_.commit();

    int row = feedsView_->currentIndex().row();
    feedsModel_->select();
    if (feedsModel_->rowCount() == row) row--;
    feedsView_->setCurrentIndex(feedsModel_->index(row, 1));
    slotFeedsTreeClicked(feedsModel_->index(row, 1));
  }
}

/*! \brief Импорт лент из OPML-файла ******************************************/
void RSSListing::slotImportFeeds()
{
  playSoundNewNews_ = false;

  QString fileName = QFileDialog::getOpenFileName(this, tr("Select OPML-file"),
                                                  QDir::homePath(),
                                                  tr("OPML-files (*.opml *.xml)"));

  if (fileName.isNull()) {
    statusBar()->showMessage(tr("Import canceled"), 3000);
    return;
  }

  qDebug() << "import file:" << fileName;

  QFile file(fileName);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    statusBar()->showMessage(tr("Import: can't open a file"), 3000);
    return;
  }

  db_.transaction();

  QXmlStreamReader xml(&file);

  int elementCount = 0;
  int outlineCount = 0;
  int requestUrlCount = 0;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isStartElement()) {
      statusBar()->showMessage(QVariant(elementCount).toString(), 3000);
      // Выбираем одни outline'ы
      if (xml.name() == "outline") {
        qDebug() << outlineCount << "+:" << xml.prefix().toString()
                 << ":" << xml.name().toString();
        QSqlQuery q(db_);

        QString textString(xml.attributes().value("text").toString());
        QString xmlUrlString(xml.attributes().value("xmlUrl").toString());
        bool duplicateFound = false;
        q.exec("select xmlUrl from feeds");
        while (q.next()) {
          if (q.record().value(0).toString() == xmlUrlString) {
            duplicateFound = true;
            break;
          }
        }

        if (duplicateFound) {
          qDebug() << "duplicate feed:" << xmlUrlString << textString;
        } else {
          QString qStr = QString("insert into feeds(text, title, description, xmlUrl, htmlUrl) "
                                 "values(?, ?, ?, ?, ?)");
          q.prepare(qStr);
          q.addBindValue(textString);
          q.addBindValue(xml.attributes().value("title").toString());
          q.addBindValue(xml.attributes().value("description").toString());
          q.addBindValue(xmlUrlString);
          q.addBindValue(xml.attributes().value("htmlUrl").toString());
          q.exec();
          qDebug() << q.lastQuery() << q.boundValues();
          qDebug() << q.lastError().number() << ": " << q.lastError().text();
//          q.exec(kCreateNewsTableQuery.arg(q.lastInsertId().toString()));
          q.finish();

          persistentUpdateThread_->requestUrl(xmlUrlString, QDateTime());
          faviconLoader->requestUrl(
                xml.attributes().value("htmlUrl").toString(), xmlUrlString);
          requestUrlCount++;
        }
      }
    } else if (xml.isEndElement()) {
      if (xml.name() == "outline") {
        ++outlineCount;
      }
      ++elementCount;
    }
  }
  if (xml.error()) {
    statusBar()->showMessage(QString("Import error: Line=%1, ErrorString=%2").
                             arg(xml.lineNumber()).arg(xml.errorString()), 3000);
  } else {
    statusBar()->showMessage(QString("Import: file read done"), 3000);
  }
  db_.commit();

  if (requestUrlCount) {
    updateAllFeedsAct_->setEnabled(false);
    updateFeedAct_->setEnabled(false);
    showProgressBar(requestUrlCount-1);
  }

  QModelIndex index = feedsView_->currentIndex();
  feedsModel_->select();
  feedsView_->setCurrentIndex(index);
}
/*! Экспорт ленты в OPML-файл *************************************************/
void RSSListing::slotExportFeeds()
{
  QString fileName = QFileDialog::getSaveFileName(this, tr("Select OPML-file"),
                                                  QDir::homePath(),
                                                  tr("OPML-files (*.opml)"));

  if (fileName.isNull()) {
    statusBar()->showMessage(tr("Export canceled"), 3000);
    return;
  }

  QFile file(fileName);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    statusBar()->showMessage(tr("Export: can't open a file"), 3000);
    return;
  }

  QXmlStreamWriter xml(&file);
  xml.setAutoFormatting(true);
  xml.writeStartDocument();
  xml.writeStartElement("opml");
  xml.writeAttribute("version", "2.0");
  xml.writeStartElement("head");
  xml.writeTextElement("title", "QuiteRSS");
  xml.writeTextElement("dateModified", QDateTime::currentDateTime().toString());
  xml.writeEndElement(); // </head>

  QSqlQuery q(db_);
  q.exec("select * from feeds where xmlUrl is not null");

  xml.writeStartElement("body");
  while (q.next()) {
    QString value = q.record().value(q.record().indexOf("text")).toString();
    xml.writeEmptyElement("outline");
    xml.writeAttribute("text", value);
    value = q.record().value(q.record().indexOf("htmlUrl")).toString();
    xml.writeAttribute("htmlUrl", value);
    value = q.record().value(q.record().indexOf("xmlUrl")).toString();
    xml.writeAttribute("xmlUrl", value);
  }
  xml.writeEndElement(); // </body>

  xml.writeEndElement(); // </opml>
  xml.writeEndDocument();

  file.close();
}
/*! \brief приём xml-файла ****************************************************/
void RSSListing::receiveXml(const QByteArray &data, const QUrl &url)
{
  url_ = url;
  data_.append(data);
}

/*! \brief Обработка окончания запроса ****************************************/
void RSSListing::getUrlDone(const int &result, const QDateTime &dtReply)
{
  qDebug() << "getUrl result =" << result;

  if (!url_.isEmpty() && !data_.isEmpty()) {
    emit xmlReadyParse(data_, url_);
    QSqlQuery q = db_.exec(QString("update feeds set lastBuildDate = '%1' where xmlUrl == '%2'").
                           arg(dtReply.toString(Qt::ISODate)).
                           arg(url_.toString()));
    qDebug() << url_.toString() << dtReply.toString(Qt::ISODate);
    qDebug() << q.lastQuery() << q.lastError() << q.lastError().text();
  }
  data_.clear();
  url_.clear();

  // очередь запросов пуста
  if (0 == result) {
    if (showMessageOn_) { // result=0 может приходить несколько раз
      statusBar()->showMessage(QString(tr("Update done")), 3000);
      showMessageOn_ = false;
    }
    updateAllFeedsAct_->setEnabled(true);
    updateFeedAct_->setEnabled(true);
    progressBar_->hide();
    progressBar_->setValue(0);
    progressBar_->setMaximum(0);
  }
  // в очереди запросов осталось _result_ запросов
  else if (0 < result) {
    progressBar_->setValue(progressBar_->maximum() - result);
    if (showMessageOn_)
      statusBar()->showMessage(progressBar_->text());
  }
}

void RSSListing::slotUpdateFeed(const QUrl &url, const bool &changed)
{
  if (!changed) return;

  // поиск идентификатора ленты в таблице лент
  int parseFeedId = 0;
  QSqlQuery q(db_);
  q.exec(QString("select id from feeds where xmlUrl like '%1'").
         arg(url.toString()));
  while (q.next()) {
    parseFeedId = q.value(q.record().indexOf("id")).toInt();
  }

  int unreadCount = 0;
  QString qStr = QString("select count(read) from feed_%1 where read==0").
      arg(parseFeedId);
  q.exec(qStr);
  if (q.next()) unreadCount = q.value(0).toInt();

  qStr = QString("update feeds set unread='%1' where id=='%2'").
      arg(unreadCount).arg(parseFeedId);
  q.exec(qStr);

  int newCount = 0;
  qStr = QString("select count(new) from feed_%1 where new==1").
      arg(parseFeedId);
  q.exec(qStr);
  if (q.next()) newCount = q.value(0).toInt();

  int newCountOld = 0;
  qStr = QString("select newCount from feeds where id=='%1'").
      arg(parseFeedId);
  q.exec(qStr);
  if (q.next()) newCountOld = q.value(0).toInt();

  qStr = QString("update feeds set newCount='%1' where id=='%2'").
      arg(newCount).arg(parseFeedId);
  q.exec(qStr);

  if (!isActiveWindow() && (newCount > newCountOld) && (behaviorIconTray_ == 1))
    traySystem->setIcon(QIcon(":/images/quiterss16_NewNews"));
  refreshInfoTray();
  if (newCount > newCountOld) {
    playSoundNewNews();
  }

  QModelIndex index = feedsView_->currentIndex();
  int id = feedsModel_->index(index.row(), feedsModel_->fieldIndex("id")).data().toInt();

  // если обновлена просматриваемая лента, кликаем по ней
  if (parseFeedId == id) {
    slotFeedsTreeSelected(feedsModel_->index(index.row(), 1));
  }
  // иначе обновляем модель лент
  else {
    feedsModel_->select();
    int rowFeeds = -1;
    for (int i = 0; i < feedsModel_->rowCount(); i++) {
      if (feedsModel_->index(i, feedsModel_->fieldIndex("id")).data().toInt() == id) {
        rowFeeds = i;
      }
    }
    feedsView_->setCurrentIndex(feedsModel_->index(rowFeeds, 1));
  }
}

/*! \brief Обработка нажатия в дереве лент ************************************/
void RSSListing::slotFeedsTreeClicked(QModelIndex index)
{
  static int idOld = -2;
  if (feedsModel_->index(index.row(), 0).data() != idOld) {
    slotFeedsTreeSelected(index, true);
  }
  idOld = feedsModel_->index(feedsView_->currentIndex().row(), 0).data().toInt();
}

void RSSListing::slotFeedsTreeSelected(QModelIndex index, bool clicked)
{
  static int idOld = feedsModel_->index(index.row(), 0).data().toInt();

  if (feedsModel_->index(index.row(), 0).data() != idOld) {
    setFeedRead(idOld);
  }

  QByteArray byteArray = feedsModel_->index(index.row(), feedsModel_->fieldIndex("image")).
      data().toByteArray();
  if (!byteArray.isNull()) {
    QPixmap icon;
    icon.loadFromData(QByteArray::fromBase64(byteArray));
    newsIconTitle_->setPixmap(icon);
  } else newsIconTitle_->setPixmap(QPixmap(":/images/feed"));
  newsTextTitle_->setText(feedsModel_->index(index.row(), 1).data().toString());

  if (index.isValid()) feedProperties_->setEnabled(true);
  else feedProperties_->setEnabled(false);

  if (index.isValid()) newsHeader_->setVisible(true);
  else newsHeader_->setVisible(false);

  setFeedsFilter(feedsFilterGroup_->checkedAction(), false);

  index = feedsView_->currentIndex();
  idOld = feedsModel_->index(index.row(), 0).data().toInt();
  bool initNo = false;
  if (newsModel_->columnCount() == 0) initNo = true;
//  newsModel_->setTable(QString("feed_%1").arg(feedsModel_->index(index.row(), 0).data().toString()));
  newsModel_->setTable("news");

  newsModel_->setSort(newsHeader_->sortIndicatorSection(),
                      newsHeader_->sortIndicatorOrder());

  setNewsFilter(newsFilterGroup_->checkedAction(), false);

  newsModel_->select();

  newsHeader_->overload();
  if (initNo) {
    newsHeader_->initColumns();
    newsHeader_->restoreGeometry(settings_->value("NewsHeaderGeometry").toByteArray());
    newsHeader_->restoreState(settings_->value("NewsHeaderState").toByteArray());
    newsHeader_->createMenu();
  }

  if (newsModel_->rowCount() != 0) {
    while (newsModel_->canFetchMore())
      newsModel_->fetchMore();
  }

  int row = -1;
  if ((openingFeedAction_ == 0) || !clicked) {
    for (int i = 0; i < newsModel_->rowCount(); i++) {
      if (newsModel_->index(i, newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt() ==
          feedsModel_->index(index.row(), feedsModel_->fieldIndex("currentNews")).data().toInt()) {
        row = i;
        break;
      }
    }
  } else if (openingFeedAction_ == 1) row = 0;

  newsView_->setCurrentIndex(newsModel_->index(row, 6));
  if ((openingFeedAction_ < 2) && openNewsWebViewOn_ && clicked) {
    slotNewsViewSelected(newsModel_->index(row, 6));
  } else if (clicked) {
    slotNewsViewSelected(newsModel_->index(-1, 6));
    QSqlQuery q(db_);
    int id = newsModel_->index(row, 0).
        data(Qt::EditRole).toInt();
    QString qStr = QString("update feeds set currentNews='%1' where id=='%2'").
        arg(id).arg(newsModel_->tableName().remove("feed_"));
    q.exec(qStr);
  } else {
    slotUpdateStatus();
  }
}

/*! \brief Обработка нажатия в дереве новостей ********************************/
void RSSListing::slotNewsViewClicked(QModelIndex index)
{
  if (QApplication::keyboardModifiers() == Qt::NoModifier) {
    if ((newsView_->selectionModel()->selectedRows(0).count() == 1)) {
      slotNewsViewSelected(index);
    }
  }
}

void RSSListing::slotNewsViewSelected(QModelIndex index)
{
  static int idxOld = -1;
  static int curFeedOld = -1;
  if (!index.isValid()) {
    webView_->setHtml("");
    webPanel_->hide();
    slotUpdateStatus();  // необходимо, когда выбрана другая лента, но новость в ней не выбрана
    idxOld = newsModel_->index(index.row(), 0).data(Qt::EditRole).toInt();
    curFeedOld = feedsModel_->index(feedsView_->currentIndex().row(), 0).data().toInt();
    return;
  }

  int idx = newsModel_->index(index.row(), 0).data(Qt::EditRole).toInt();
  int curFeed = feedsModel_->index(feedsView_->currentIndex().row(), 0).data().toInt();

  if (!((idx == idxOld) && (curFeed == curFeedOld) &&
        newsModel_->index(index.row(), newsModel_->fieldIndex("read")).data(Qt::EditRole).toInt() >= 1) ||
      (QApplication::mouseButtons() & Qt::MiddleButton)) {

    QWebSettings::globalSettings()->clearMemoryCaches();
    webView_->history()->clear();

    updateWebView(index);

    markNewsReadTimer_.stop();
    if (markNewsReadOn_)
      markNewsReadTimer_.start(markNewsReadTime_*1000, this);

    QSqlQuery q(db_);
    int id = newsModel_->index(index.row(), 0).
        data(Qt::EditRole).toInt();
    QString qStr = QString("update feeds set currentNews='%1' where id=='%2'").
        arg(id).arg(newsModel_->tableName().remove("feed_"));
    q.exec(qStr);
  } else slotUpdateStatus();

  idxOld = idx;
  curFeedOld = curFeed;
}

/*! \brief Вызов окна настроек ************************************************/
void RSSListing::showOptionDlg()
{
  static int index = 0;
  OptionsDialog *optionsDialog = new OptionsDialog(this);
  optionsDialog->restoreGeometry(settings_->value("options/geometry").toByteArray());
  optionsDialog->setCurrentItem(index);

  optionsDialog->showSplashScreen_->setChecked(showSplashScreen_);
  optionsDialog->reopenFeedStartup_->setChecked(reopenFeedStartup_);

  optionsDialog->showTrayIconBox_->setChecked(showTrayIcon_);
  optionsDialog->startingTray_->setChecked(startingTray_);
  optionsDialog->minimizingTray_->setChecked(minimizingTray_);
  optionsDialog->closingTray_->setChecked(closingTray_);
  optionsDialog->setBehaviorIconTray(behaviorIconTray_);
  optionsDialog->singleClickTray_->setChecked(singleClickTray_);
  optionsDialog->clearStatusNew_->setChecked(clearStatusNew_);
  optionsDialog->emptyWorking_->setChecked(emptyWorking_);

  optionsDialog->setProxy(networkProxy_);

  optionsDialog->embeddedBrowserOn_->setChecked(embeddedBrowserOn_);
  optionsDialog->javaScriptEnable_->setChecked(javaScriptEnable_);
  optionsDialog->pluginsEnable_->setChecked(pluginsEnable_);

  optionsDialog->updateFeedsStartUp_->setChecked(autoUpdatefeedsStartUp_);
  optionsDialog->updateFeeds_->setChecked(autoUpdatefeeds_);
  optionsDialog->intervalTime_->setCurrentIndex(autoUpdatefeedsInterval_);
  optionsDialog->updateFeedsTime_->setValue(autoUpdatefeedsTime_);

  optionsDialog->setOpeningFeed(openingFeedAction_);
  optionsDialog->openNewsWebViewOn_->setChecked(openNewsWebViewOn_);

  optionsDialog->markNewsReadOn_->setChecked(markNewsReadOn_);
  optionsDialog->markNewsReadTime_->setValue(markNewsReadTime_);

  optionsDialog->showDescriptionNews_->setChecked(showDescriptionNews_);

  optionsDialog->dayCleanUpOn_->setChecked(dayCleanUpOn_);
  optionsDialog->maxDayCleanUp_->setValue(maxDayCleanUp_);
  optionsDialog->newsCleanUpOn_->setChecked(newsCleanUpOn_);
  optionsDialog->maxNewsCleanUp_->setValue(maxNewsCleanUp_);
  optionsDialog->readCleanUp_->setChecked(readCleanUp_);
  optionsDialog->neverUnreadCleanUp_->setChecked(neverUnreadCleanUp_);
  optionsDialog->neverStarCleanUp_->setChecked(neverStarCleanUp_);

  optionsDialog->soundNewNews_->setChecked(soundNewNews_);

  optionsDialog->setLanguage(langFileName_);

  QString strFont = QString("%1, %2").
      arg(feedsView_->font().family()).
      arg(feedsView_->font().pointSize());
  optionsDialog->fontTree->topLevelItem(0)->setText(2, strFont);
  strFont = QString("%1, %2").
      arg(newsView_->font().family()).
      arg(newsView_->font().pointSize());
  optionsDialog->fontTree->topLevelItem(1)->setText(2, strFont);
  strFont = QString("%1, %2").
      arg(webView_->settings()->fontFamily(QWebSettings::StandardFont)).
      arg(webView_->settings()->fontSize(QWebSettings::DefaultFontSize));
  optionsDialog->fontTree->topLevelItem(2)->setText(2, strFont);

  optionsDialog->loadActionShortcut(listActions_, &listDefaultShortcut_);
//
  int result = optionsDialog->exec();
  settings_->setValue("options/geometry", optionsDialog->saveGeometry());
  index = optionsDialog->currentIndex();

  if (result == QDialog::Rejected) {
    delete optionsDialog;
    return;
  }

  showSplashScreen_ = optionsDialog->showSplashScreen_->isChecked();
  reopenFeedStartup_ = optionsDialog->reopenFeedStartup_->isChecked();

  showTrayIcon_ = optionsDialog->showTrayIconBox_->isChecked();
  startingTray_ = optionsDialog->startingTray_->isChecked();
  minimizingTray_ = optionsDialog->minimizingTray_->isChecked();
  closingTray_ = optionsDialog->closingTray_->isChecked();
  behaviorIconTray_ = optionsDialog->behaviorIconTray();
  if (behaviorIconTray_ > 1) {
    refreshInfoTray();
  } else traySystem->setIcon(QIcon(":/images/quiterss16"));
  singleClickTray_ = optionsDialog->singleClickTray_->isChecked();
  clearStatusNew_ = optionsDialog->clearStatusNew_->isChecked();
  emptyWorking_ = optionsDialog->emptyWorking_->isChecked();
  if (showTrayIcon_) traySystem->show();
  else traySystem->hide();

  networkProxy_ = optionsDialog->proxy();
  persistentUpdateThread_->setProxy(networkProxy_);

  embeddedBrowserOn_ = optionsDialog->embeddedBrowserOn_->isChecked();
  if (embeddedBrowserOn_) {
    webView_->page()->setLinkDelegationPolicy(QWebPage::DelegateAllLinks);
    openInExternalBrowserAct_->setVisible(true);
  } else {
    webView_->page()->setLinkDelegationPolicy(QWebPage::DelegateExternalLinks);
    openInExternalBrowserAct_->setVisible(false);
  }
  javaScriptEnable_ = optionsDialog->javaScriptEnable_->isChecked();
  webView_->settings()->setAttribute(
        QWebSettings::JavascriptEnabled, javaScriptEnable_);
  pluginsEnable_ = optionsDialog->pluginsEnable_->isChecked();
  webView_->settings()->setAttribute(
        QWebSettings::PluginsEnabled, pluginsEnable_);

  autoUpdatefeedsStartUp_ = optionsDialog->updateFeedsStartUp_->isChecked();
  autoUpdatefeeds_ = optionsDialog->updateFeeds_->isChecked();
  autoUpdatefeedsTime_ = optionsDialog->updateFeedsTime_->value();
  autoUpdatefeedsInterval_ = optionsDialog->intervalTime_->currentIndex();
  if (updateFeedsTimer_.isActive() && !autoUpdatefeeds_) {
    updateFeedsTimer_.stop();
  } else if (!updateFeedsTimer_.isActive() && autoUpdatefeeds_) {
    int updateFeedsTime = autoUpdatefeedsTime_*60000;
    if (autoUpdatefeedsInterval_ == 1)
      updateFeedsTime = updateFeedsTime*60;
    updateFeedsTimer_.start(updateFeedsTime, this);
  }

  openingFeedAction_ = optionsDialog->getOpeningFeed();
  openNewsWebViewOn_ = optionsDialog->openNewsWebViewOn_->isChecked();

  markNewsReadOn_ = optionsDialog->markNewsReadOn_->isChecked();
  markNewsReadTime_ = optionsDialog->markNewsReadTime_->value();

  showDescriptionNews_ = optionsDialog->showDescriptionNews_->isChecked();

  dayCleanUpOn_ = optionsDialog->dayCleanUpOn_->isChecked();
  maxDayCleanUp_ = optionsDialog->maxDayCleanUp_->value();
  newsCleanUpOn_ = optionsDialog->newsCleanUpOn_->isChecked();
  maxNewsCleanUp_ = optionsDialog->maxNewsCleanUp_->value();
  readCleanUp_ = optionsDialog->readCleanUp_->isChecked();
  neverUnreadCleanUp_ = optionsDialog->neverUnreadCleanUp_->isChecked();
  neverStarCleanUp_ = optionsDialog->neverStarCleanUp_->isChecked();

  soundNewNews_ = optionsDialog->soundNewNews_->isChecked();

  if (langFileName_ != optionsDialog->language()) {
    langFileName_ = optionsDialog->language();
    appInstallTranslator();
  }

  QFont font = feedsView_->font();
  font.setFamily(
        optionsDialog->fontTree->topLevelItem(0)->text(2).section(", ", 0, 0));
  font.setPointSize(
        optionsDialog->fontTree->topLevelItem(0)->text(2).section(", ", 1).toInt());
  feedsView_->setFont(font);
  feedsModel_->font_ = font;
  font = newsView_->font();
  font.setFamily(
        optionsDialog->fontTree->topLevelItem(1)->text(2).section(", ", 0, 0));
  font.setPointSize(
        optionsDialog->fontTree->topLevelItem(1)->text(2).section(", ", 1).toInt());
  newsView_->setFont(font);
  webView_->settings()->setFontFamily(QWebSettings::StandardFont,
                                      optionsDialog->fontTree->topLevelItem(2)->text(2).section(", ", 0, 0));
  webView_->settings()->setFontSize(QWebSettings::DefaultFontSize,
                                    optionsDialog->fontTree->topLevelItem(2)->text(2).section(", ", 1).toInt());
  optionsDialog->saveActionShortcut(listActions_);

  delete optionsDialog;
  writeSettings();
  saveActionShortcuts();
}

/*! \brief Обработка сообщений полученных из запущщеной копии программы *******/
void RSSListing::receiveMessage(const QString& message)
{
  qDebug() << QString("Received message: '%1'").arg(message);
  if (!message.isEmpty()){
    QStringList params = message.split('\n');
    foreach (QString param, params) {
      if (param == "--show") slotShowWindows();
      if (param == "--exit") slotClose();
    }
  }
}

/*! \brief Создание меню трея *************************************************/
void RSSListing::createTrayMenu()
{
  trayMenu_ = new QMenu(this);
  showWindowAct_ = new QAction(this);
  connect(showWindowAct_, SIGNAL(triggered()), this, SLOT(slotShowWindows()));
  QFont font_ = showWindowAct_->font();
  font_.setBold(true);
  showWindowAct_->setFont(font_);
  trayMenu_->addAction(showWindowAct_);
  trayMenu_->addAction(updateAllFeedsAct_);
  trayMenu_->addSeparator();

  trayMenu_->addAction(optionsAct_);
  trayMenu_->addSeparator();

  trayMenu_->addAction(exitAct_);
  traySystem->setContextMenu(trayMenu_);
}

/*! \brief Освобождение памяти ************************************************/
void RSSListing::myEmptyWorkingSet()
{
#if defined(Q_WS_WIN)
  if (isHidden())
    EmptyWorkingSet(GetCurrentProcess());
#endif
}

/*! \brief Показ статус бара после запрос обновления ленты ********************/
void RSSListing::showProgressBar(int addToMaximum)
{
  progressBar_->setMaximum(progressBar_->maximum() + addToMaximum);
  showMessageOn_ = true;
  statusBar()->showMessage(progressBar_->text());
  progressBar_->show();
  QTimer::singleShot(150, this, SLOT(slotProgressBarUpdate()));
  emit startGetUrlTimer();
}

/*! \brief Обновление ленты (действие) ****************************************/
void RSSListing::slotGetFeed()
{
  playSoundNewNews_ = false;

  persistentUpdateThread_->requestUrl(
        feedsModel_->record(feedsView_->currentIndex().row()).field("xmlUrl").value().toString(),
        QDateTime::fromString(feedsModel_->record(feedsView_->currentIndex().row()).field("lastBuildDate").value().toString(), Qt::ISODate)
        );
  showProgressBar(1);
}

/*! \brief Обновление ленты (действие) ****************************************/
void RSSListing::slotGetAllFeeds()
{
  int feedCount = 0;

  playSoundNewNews_ = false;

  QSqlQuery q(db_);
  q.exec("select xmlUrl, lastBuildDate from feeds where xmlUrl is not null");
  qDebug() << q.lastError();
  while (q.next()) {
    persistentUpdateThread_->requestUrl(q.record().value(0).toString(),
                                        q.record().value(1).toDateTime());
    ++feedCount;
  }

  if (feedCount) {
    updateAllFeedsAct_->setEnabled(false);
    updateFeedAct_->setEnabled(false);
    showProgressBar(feedCount - 1);
  }
}

void RSSListing::slotProgressBarUpdate()
{
  progressBar_->update();

  if (progressBar_->isVisible())
    QTimer::singleShot(150, this, SLOT(slotProgressBarUpdate()));
}

void RSSListing::slotVisibledFeedsDock()
{
  feedsDock_->setVisible(!feedsDock_->isVisible());
  if (newsDockArea_ == feedsDockArea_)
    newsDock_->setVisible(feedsDock_->isVisible());
}

void RSSListing::updateIconToolBarNull(bool feedsDockVisible)
{
  if (feedsDockArea_ == Qt::LeftDockWidgetArea) {
    if (feedsDockVisible)
      pushButtonNull_->setIcon(QIcon(":/images/images/triangleR.png"));
    else
      pushButtonNull_->setIcon(QIcon(":/images/images/triangleL.png"));
  } else if (feedsDockArea_ == Qt::RightDockWidgetArea) {
    if (feedsDockVisible)
      pushButtonNull_->setIcon(QIcon(":/images/images/triangleL.png"));
    else
      pushButtonNull_->setIcon(QIcon(":/images/images/triangleR.png"));
  }
}

void RSSListing::slotDockLocationChanged(Qt::DockWidgetArea area)
{
  if (area == Qt::LeftDockWidgetArea) {
    toolBarNull_->show();
    addToolBar(Qt::LeftToolBarArea, toolBarNull_);
  } else if (area == Qt::RightDockWidgetArea) {
    toolBarNull_->show();
    addToolBar(Qt::RightToolBarArea, toolBarNull_);
  } else {
    toolBarNull_->hide();
  }
  updateIconToolBarNull(feedsDock_->isVisible());
}

void RSSListing::slotSetItemRead(QModelIndex index, int read)
{
  if (!index.isValid()) return;

  if (newsModel_->rowCount() != 0) {
    int topRow = newsView_->verticalScrollBar()->value();
    QModelIndex curIndex = newsView_->currentIndex();
    if (newsModel_->index(index.row(), newsModel_->fieldIndex("new")).data(Qt::EditRole).toInt() == 1) {
      newsModel_->setData(
            newsModel_->index(index.row(), newsModel_->fieldIndex("new")),
            0);
    }
    if (read == 1) {
      if (newsModel_->index(index.row(), newsModel_->fieldIndex("read")).data(Qt::EditRole).toInt() == 0) {
        newsModel_->setData(
              newsModel_->index(index.row(), newsModel_->fieldIndex("read")),
              1);
      }
    } else {
      newsModel_->setData(
            newsModel_->index(index.row(), newsModel_->fieldIndex("read")),
            0);
    }

    while (newsModel_->canFetchMore())
      newsModel_->fetchMore();

    if (newsView_->currentIndex() != curIndex)
      newsView_->setCurrentIndex(curIndex);
    newsView_->verticalScrollBar()->setValue(topRow);
  }
  slotUpdateStatus();
}

void RSSListing::markNewsRead()
{
  QModelIndex curIndex;
  QList<QModelIndex> indexes = newsView_->selectionModel()->selectedRows(0);

  int cnt = indexes.count();
  if (cnt == 0) return;

  if (cnt == 1) {
    curIndex = indexes.at(0);
    if (newsModel_->index(curIndex.row(), newsModel_->fieldIndex("read")).data(Qt::EditRole).toInt() == 0) {
      slotSetItemRead(curIndex, 1);
    } else {
      slotSetItemRead(curIndex, 0);
    }
  } else {
    bool markRead = false;
    for (int i = cnt-1; i >= 0; --i) {
      curIndex = indexes.at(i);
      if (newsModel_->index(curIndex.row(), newsModel_->fieldIndex("read")).data(Qt::EditRole).toInt() == 0) {
        markRead = true;
        break;
      }
    }

    for (int i = cnt-1; i >= 0; --i) {
      curIndex = indexes.at(i);
      QSqlQuery q(db_);
      q.exec(QString("update %1 set new=0 where id=='%2'").
             arg(newsModel_->tableName()).
             arg(newsModel_->index(curIndex.row(), newsModel_->fieldIndex("id")).data().toInt()));
      q.exec(QString("update %1 set read='%2' where id=='%3'").
             arg(newsModel_->tableName()).arg(markRead).
             arg(newsModel_->index(curIndex.row(), newsModel_->fieldIndex("id")).data().toInt()));
    }

    newsModel_->select();

    while (newsModel_->canFetchMore())
      newsModel_->fetchMore();

    int row = -1;
    for (int i = 0; i < newsModel_->rowCount(); i++) {
      if (newsModel_->index(i, newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt() ==
          feedsModel_->index(feedsView_->currentIndex().row(),
                             feedsModel_->fieldIndex("currentNews")).data().toInt()) {
        row = i;
        break;
      }
    }
    newsView_->setCurrentIndex(newsModel_->index(row, 6));
    slotUpdateStatus();
  }
}

void RSSListing::markAllNewsRead()
{
  if (newsModel_->rowCount() == 0) return;

  int currentRow = newsView_->currentIndex().row();
  QString qStr = QString("update %1 set read=1 WHERE read=0").
      arg(newsModel_->tableName());
  QSqlQuery q(db_);
  q.exec(qStr);
  qStr = QString("UPDATE %1 SET new=0 WHERE new=1").
      arg(newsModel_->tableName());
  q.exec(qStr);

  setNewsFilter(newsFilterGroup_->checkedAction(), false);
  newsModel_->select();

  while (newsModel_->canFetchMore())
    newsModel_->fetchMore();

  int row = -1;
  for (int i = 0; i < newsModel_->rowCount(); i++) {
    if (newsModel_->index(i, newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt() ==
        feedsModel_->index(feedsView_->currentIndex().row(),
                           feedsModel_->fieldIndex("currentNews")).data().toInt()) {
      row = i;
      break;
    }
  }
  newsView_->setCurrentIndex(newsModel_->index(row, 6));
  if (currentRow != row)
    updateWebView(newsModel_->index(row, 6));
  slotUpdateStatus();
}

/*! \brief Подсчёт новостей
 *
 * Подсчёт всех новостей в фиде. (50мс)
 * Подсчёт всех не прочитанных новостей в фиде. (50мс)
 * Подсчёт всех новых новостей в фиде. (50мс)
 * Запись этих данных в таблицу лент (100мс)
 * Вывод этих данных в статусную строку
 */
void RSSListing::slotUpdateStatus()
{
  QString qStr;

  int id = feedsModel_->index(
        feedsView_->currentIndex().row(), feedsModel_->fieldIndex("id")).data(Qt::EditRole).toInt();

  int allCount = 0;
  qStr = QString("select count(id) from %1 where deleted=0").
      arg(newsModel_->tableName());
  QSqlQuery q(db_);
  q.exec(qStr);
  if (q.next()) allCount = q.value(0).toInt();

  int unreadCount = 0;
  qStr = QString("select count(read) from %1 where read==0").
      arg(newsModel_->tableName());
  q.exec(qStr);
  if (q.next()) unreadCount = q.value(0).toInt();

  int newCount = 0;
  qStr = QString("select count(new) from %1 where new==1").
      arg(newsModel_->tableName());
  q.exec(qStr);
  if (q.next()) newCount = q.value(0).toInt();

  int newCountOld = 0;
  qStr = QString("select newCount from feeds where id=='%1'").
      arg(newsModel_->tableName().remove("feed_"));
  q.exec(qStr);
  if (q.next()) newCountOld = q.value(0).toInt();

  qStr = QString("update feeds set unread='%1', newCount='%2' where id=='%3'").
      arg(unreadCount).arg(newCount).
      arg(newsModel_->tableName().remove("feed_"));
  q.exec(qStr);

  if (!isActiveWindow() && (newCount > newCountOld) && (behaviorIconTray_ == 1))
    traySystem->setIcon(QIcon(":/images/quiterss16_NewNews"));
  refreshInfoTray();
  if (newCount > newCountOld) {
    playSoundNewNews();
  }

  feedsModel_->select();
  int rowFeeds = -1;
  for (int i = 0; i < feedsModel_->rowCount(); i++) {
    if (feedsModel_->index(i, feedsModel_->fieldIndex("id")).data().toInt() == id) {
      rowFeeds = i;
    }
  }
  feedsView_->setCurrentIndex(feedsModel_->index(rowFeeds, 1));

  statusUnread_->setText(QString(tr(" Unread: %1 ")).arg(unreadCount));
  statusAll_->setText(QString(tr(" All: %1 ")).arg(allCount));
}

void RSSListing::slotLoadStarted()
{
  if (newsView_->currentIndex().isValid()) {
    webViewProgress_->setValue(0);
    webViewProgress_->show();
  }
}

void RSSListing::slotLoadFinished(bool ok)
{
  if (!ok) statusBar()->showMessage(tr("Error loading to WebView"), 3000);
  webViewProgress_->hide();
}

void RSSListing::setFeedsFilter(QAction* pAct, bool clicked)
{
  QModelIndex index = feedsView_->currentIndex();

  int id = feedsModel_->index(
        index.row(), feedsModel_->fieldIndex("id")).data(Qt::EditRole).toInt();
  int unread = feedsModel_->index(
        index.row(), feedsModel_->fieldIndex("unread")).data(Qt::EditRole).toInt();
  int newCount = feedsModel_->index(
        index.row(), feedsModel_->fieldIndex("newCount")).data(Qt::EditRole).toInt();

  if (pAct->objectName() == "filterFeedsAll_") {
    feedsModel_->setFilter("");
  } else if (pAct->objectName() == "filterFeedsNew_") {
    if (clicked) {
      if (newCount)
        feedsModel_->setFilter(QString("newCount > 0 OR id == '%1'").arg(id));
      else
        feedsModel_->setFilter(QString("newCount > 0"));
    } else
      feedsModel_->setFilter(QString("newCount > 0 OR id == '%1'").arg(id));
  } else if (pAct->objectName() == "filterFeedsUnread_") {
    if (clicked) {
      if (unread > 0)
        feedsModel_->setFilter(QString("unread > 0 OR id == '%1'").arg(id));
      else
        feedsModel_->setFilter(QString("unread > 0"));
    } else
      feedsModel_->setFilter(QString("unread > 0 OR id == '%1'").arg(id));
  }

  if (pAct->objectName() == "filterFeedsAll_") feedsFilter_->setIcon(QIcon(":/images/filterOff"));
  else feedsFilter_->setIcon(QIcon(":/images/filterOn"));

  int rowFeeds = -1;
  for (int i = 0; i < feedsModel_->rowCount(); i++) {
    if (feedsModel_->index(i, feedsModel_->fieldIndex("id")).data(Qt::EditRole).toInt() == id) {
      rowFeeds = i;
    }
  }
  feedsView_->setCurrentIndex(feedsModel_->index(rowFeeds, 1));

  if (pAct->objectName() != "filterFeedsAll_")
    feedsFilterAction = pAct;
}

void RSSListing::setNewsFilter(QAction* pAct, bool clicked)
{
  QModelIndex index = newsView_->currentIndex();

  int id = newsModel_->index(
        index.row(), newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt();

  if (clicked) {
    QString qStr = QString("UPDATE %1 SET read=2 WHERE read=1").
        arg(newsModel_->tableName());
    QSqlQuery q(db_);
    q.exec(qStr);
  }

  int feedId = feedsModel_->index(
        feedsView_->currentIndex().row(), feedsModel_->fieldIndex("id")).data(Qt::EditRole).toInt();
  QString feedIdFilter(QString("feedId=%1 AND ").arg(feedId));
  if (pAct->objectName() == "filterNewsAll_") {
    newsModel_->setFilter(feedIdFilter.append("deleted = 0"));
  } else if (pAct->objectName() == "filterNewsNew_") {
    newsModel_->setFilter(feedIdFilter.append(QString("new = 1 AND deleted = 0")));
  } else if (pAct->objectName() == "filterNewsUnread_") {
    newsModel_->setFilter(feedIdFilter.append(QString("read < 2 AND deleted = 0")));
  } else if (pAct->objectName() == "filterNewsStar_") {
    newsModel_->setFilter(feedIdFilter.append(QString("starred = 1 AND deleted = 0")));
  }

  if (pAct->objectName() == "filterNewsAll_") newsFilter_->setIcon(QIcon(":/images/filterOff"));
  else newsFilter_->setIcon(QIcon(":/images/filterOn"));

  if (clicked) {
    int row = -1;
    for (int i = 0; i < newsModel_->rowCount(); i++) {
      if (newsModel_->index(i, newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt() == id) {
        row = i;
      }
    }
    newsView_->setCurrentIndex(newsModel_->index(row, 6));
    if (row == -1) {
      webView_->setHtml("");
      webPanel_->hide();
    }
  }

  if (pAct->objectName() != "filterNewsAll_")
    newsFilterAction = pAct;
}

void RSSListing::slotFeedsDockLocationChanged(Qt::DockWidgetArea area)
{
  feedsDockArea_ = area;
}

void RSSListing::slotNewsDockLocationChanged(Qt::DockWidgetArea area)
{
  newsDockArea_ = area;
}

void RSSListing::slotNewsViewDoubleClicked(QModelIndex index)
{
  if (!index.isValid()) return;

  QString linkString = newsModel_->record(
        index.row()).field("link_href").value().toString();
  if (linkString.isEmpty())
    linkString = newsModel_->record(index.row()).field("link_alternate").value().toString();

  if (embeddedBrowserOn_) {
    webView_->history()->clear();
    webControlPanel_->setVisible(true);
    webView_->load(QUrl(linkString.simplified()));
  } else QDesktopServices::openUrl(QUrl(linkString.simplified()));
}

//! Маркировка ленты прочитанной при клике на не отмеченной ленте
void RSSListing::setFeedRead(int feedId)
{
  db_.transaction();
  QSqlQuery q(db_);
  q.exec(QString("UPDATE news SET read=2 WHERE feedId=='%1' AND read==1").arg(feedId));
  q.exec(QString("UPDATE news SET new=0 WHERE feedId=='%1' AND new==1").arg(feedId));

  q.exec(QString("UPDATE feeds SET newCount=0 WHERE id=='%1'").arg(feedId));
  db_.commit();
}

void RSSListing::slotShowAboutDlg()
{
  AboutDialog *aboutDialog = new AboutDialog(this);
  aboutDialog->exec();
  delete aboutDialog;
}

void RSSListing::deleteNews()
{
  QModelIndex curIndex;
  QList<QModelIndex> indexes = newsView_->selectionModel()->selectedRows(0);

  int cnt = indexes.count();
  if (cnt == 0) return;

  if (cnt == 1) {
    curIndex = indexes.at(0);
    int row = curIndex.row();
    slotSetItemRead(curIndex, 1);

    newsModel_->setData(
          newsModel_->index(row, newsModel_->fieldIndex("deleted")), 1);
  } else {
    for (int i = cnt-1; i >= 0; --i) {
      curIndex = indexes.at(i);
      QSqlQuery q(db_);
      q.exec(QString("update %1 set new=0 where id=='%2'").
             arg(newsModel_->tableName()).
             arg(newsModel_->index(curIndex.row(), newsModel_->fieldIndex("id")).
                 data().toInt()));
      q.exec(QString("update %1 set read=2 where id=='%2'").
             arg(newsModel_->tableName()).
             arg(newsModel_->index(curIndex.row(), newsModel_->fieldIndex("id")).
                 data().toInt()));
      q.exec(QString("update %1 set deleted=1 where id=='%2'").
             arg(newsModel_->tableName()).
             arg(newsModel_->index(curIndex.row(), newsModel_->fieldIndex("id")).
                 data().toInt()));
    }
    newsModel_->select();
  }

  while (newsModel_->canFetchMore())
    newsModel_->fetchMore();

  if (curIndex.row() == newsModel_->rowCount())
    curIndex = newsModel_->index(curIndex.row()-1, 6);
  else curIndex = newsModel_->index(curIndex.row(), 6);
  newsView_->setCurrentIndex(curIndex);
  slotNewsViewSelected(curIndex);
  slotUpdateStatus();
}

void RSSListing::createMenuNews()
{
  newsContextMenu_ = new QMenu(this);
  newsContextMenu_->addAction(openInBrowserAct_);
  newsContextMenu_->addAction(openInExternalBrowserAct_);
  newsContextMenu_->addSeparator();
  newsContextMenu_->addAction(markNewsRead_);
  newsContextMenu_->addAction(markAllNewsRead_);
  newsContextMenu_->addSeparator();
  newsContextMenu_->addAction(markStarAct_);
  newsContextMenu_->addSeparator();
  newsContextMenu_->addAction(updateFeedAct_);
  newsContextMenu_->addSeparator();
  newsContextMenu_->addAction(deleteNewsAct_);
}

void RSSListing::showContextMenuNews(const QPoint &p)
{
  if (newsView_->currentIndex().isValid())
    newsContextMenu_->popup(newsView_->viewport()->mapToGlobal(p));
}

void RSSListing::openInBrowserNews()
{
  slotNewsViewDoubleClicked(newsView_->currentIndex());
}

void RSSListing::openInExternalBrowserNews()
{
  QModelIndex index = newsView_->currentIndex();

  if (!index.isValid()) return;

  QString linkString = newsModel_->record(
        index.row()).field("link_href").value().toString();
  if (linkString.isEmpty())
    linkString = newsModel_->record(index.row()).field("link_alternate").value().toString();

  QDesktopServices::openUrl(QUrl(linkString.simplified()));
}

void RSSListing::slotSetItemStar(QModelIndex index, int starred)
{
  if (!index.isValid()) return;

  int topRow = newsView_->verticalScrollBar()->value();
  QModelIndex curIndex = newsView_->currentIndex();
  newsModel_->setData(
        newsModel_->index(index.row(), newsModel_->fieldIndex("starred")),
        starred);

  while (newsModel_->canFetchMore())
    newsModel_->fetchMore();

  newsView_->setCurrentIndex(curIndex);
  newsView_->verticalScrollBar()->setValue(topRow);
}

void RSSListing::markNewsStar()
{
  QModelIndex curIndex;
  QList<QModelIndex> indexes = newsView_->selectionModel()->selectedRows(0);

  int cnt = indexes.count();

  if (cnt == 1) {
    curIndex = indexes.at(0);
    if (newsModel_->index(curIndex.row(), newsModel_->fieldIndex("starred")).data(Qt::EditRole).toInt() == 0) {
      slotSetItemStar(curIndex, 1);
    } else {
      slotSetItemStar(curIndex, 0);
    }
  } else {
    bool markStar = false;
    for (int i = cnt-1; i >= 0; --i) {
      curIndex = indexes.at(i);
      if (newsModel_->index(curIndex.row(), newsModel_->fieldIndex("starred")).data(Qt::EditRole).toInt() == 0)
        markStar = true;
    }

    for (int i = cnt-1; i >= 0; --i) {
      curIndex = indexes.at(i);
      if (markStar) slotSetItemStar(curIndex, 1);
      else slotSetItemStar(curIndex, 0);
    }
  }
}

void RSSListing::createMenuFeed()
{
  feedContextMenu_ = new QMenu(this);
  feedContextMenu_->addAction(addFeedAct_);
  feedContextMenu_->addSeparator();
  feedContextMenu_->addAction(markFeedRead_);
  feedContextMenu_->addAction(markAllFeedRead_);
  feedContextMenu_->addSeparator();
  feedContextMenu_->addAction(updateFeedAct_);
  //  feedContextMenu_->addSeparator();
  //  feedContextMenu_->addAction(setFilterNewsAct_);
  feedContextMenu_->addSeparator();
  feedContextMenu_->addAction(deleteFeedAct_);
  feedContextMenu_->addSeparator();
  feedContextMenu_->addAction(feedProperties_);
}

void RSSListing::showContextMenuFeed(const QPoint &p)
{
  if (feedsView_->currentIndex().isValid())
    feedContextMenu_->popup(feedsView_->viewport()->mapToGlobal(p));
}

void RSSListing::slotLinkClicked(QUrl url)
{
  if (url.host().isEmpty()) {
    QUrl hostUrl(feedsModel_->record(feedsView_->currentIndex().row()).
                 field("htmlUrl").value().toUrl());
    url.setScheme(hostUrl.scheme());
    url.setHost(hostUrl.host());
  }
  if (embeddedBrowserOn_) {
    if (!webControlPanel_->isVisible()) {
      webView_->history()->clear();
      webControlPanel_->setVisible(true);
    }
    webView_->load(url);
  } else QDesktopServices::openUrl(url);
}

void RSSListing::slotLinkHovered(const QString &link, const QString &, const QString &)
{
  statusBar()->showMessage(link, 3000);
}

void RSSListing::slotSetValue(int value)
{
  webViewProgress_->setValue(value);
  QString str = QString(" %1 kB / %2 kB").
      arg(webView_->page()->bytesReceived()/1000).
      arg(webView_->page()->totalBytes()/1000);
  webViewProgressLabel_->setText(str);
}

void RSSListing::setAutoLoadImages()
{
  autoLoadImages_ = !autoLoadImages_;
  if (autoLoadImages_) {
    autoLoadImagesToggle_->setText(tr("Load images"));
    autoLoadImagesToggle_->setToolTip(tr("Auto load images to news view"));
    autoLoadImagesToggle_->setIcon(QIcon(":/images/imagesOn"));
  } else {
    autoLoadImagesToggle_->setText(tr("No load images"));
    autoLoadImagesToggle_->setToolTip(tr("No load images to news view"));
    autoLoadImagesToggle_->setIcon(QIcon(":/images/imagesOff"));
  }
  webView_->settings()->setAttribute(QWebSettings::AutoLoadImages, autoLoadImages_);
  if (webView_->history()->count() == 0)
    updateWebView(newsView_->currentIndex());
  else webView_->reload();
}

void RSSListing::loadSettingsFeeds()
{
  markNewsReadOn_ = false;
  behaviorIconTray_ = settings_->value("Settings/behaviorIconTray", 2).toInt();
  autoLoadImages_ = !settings_->value("Settings/autoLoadImages", true).toBool();
  setAutoLoadImages();

  QString filterName = settings_->value("feedSettings/filterName", "filterFeedsAll_").toString();
  QList<QAction*> listActions = feedsFilterGroup_->actions();
  foreach(QAction *action, listActions) {
    if (action->objectName() == filterName) {
      action->setChecked(true);
      break;
    }
  }
  filterName = settings_->value("newsSettings/filterName", "filterNewsAll_").toString();
  listActions = newsFilterGroup_->actions();
  foreach(QAction *action, listActions) {
    if (action->objectName() == filterName) {
      action->setChecked(true);
      break;
    }
  }

  setFeedsFilter(feedsFilterGroup_->checkedAction(), false);
}

void RSSListing::setCurrentFeed()
{
  if (reopenFeedStartup_) {
    qApp->processEvents();
    int id = settings_->value("feedSettings/currentId", 0).toInt();
    int row = -1;
    for (int i = 0; i < feedsModel_->rowCount(); i++) {
      if (feedsModel_->index(i, 0).data().toInt() == id) {
        row = i;
      }
    }
    feedsView_->setCurrentIndex(feedsModel_->index(row, 1));
    slotFeedsTreeSelected(feedsModel_->index(row, 1), true);  // загрузка новостей
  } else slotUpdateStatus();
}

void RSSListing::updateWebView(QModelIndex index)
{
  if (!index.isValid()) {
    webView_->setHtml("");
    webPanel_->hide();
    return;
  }

  webPanel_->show();

  QString titleString, linkString, panelTitleString;
  titleString = newsModel_->record(index.row()).field("title").value().toString();
  if (isVisible())
    titleString = webPanelTitle_->fontMetrics().elidedText(
          titleString, Qt::ElideRight, webPanelTitle_->width());
  linkString = newsModel_->record(index.row()).field("link_href").value().toString();
  if (linkString.isEmpty())
    linkString = newsModel_->record(index.row()).field("link_alternate").value().toString();
  panelTitleString = QString("<a href='%1'>%2</a>").arg(linkString).arg(titleString);
  webPanelTitle_->setText(panelTitleString);

  // Формируем панель автора из автора новости
  QString authorString;
  QString authorName = newsModel_->record(index.row()).field("author_name").value().toString();
  QString authorEmail = newsModel_->record(index.row()).field("author_email").value().toString();
  QString authorUri = newsModel_->record(index.row()).field("author_uri").value().toString();
  //  qDebug() << "author_news:" << authorName << authorEmail << authorUri;
  authorString = authorName;
  if (!authorEmail.isEmpty()) authorString.append(QString(" <a href='mailto:%1'>e-mail</a>").arg(authorEmail));
  if (!authorUri.isEmpty())   authorString.append(QString(" <a href='%1'>page</a>").arg(authorUri));

  // Если авора новости нет, формируем панель автора из автора ленты
  // @NOTE(arhohryakov:2012.01.03) Автор берётся из текущего фида, т.к. при
  //   новость обновляется именно у него
  if (authorString.isEmpty()) {
    authorName = feedsModel_->record(feedsView_->currentIndex().row()).field("author_name").value().toString();
    authorEmail = feedsModel_->record(feedsView_->currentIndex().row()).field("author_email").value().toString();
    authorUri = feedsModel_->record(feedsView_->currentIndex().row()).field("author_uri").value().toString();
    //    qDebug() << "author_feed:" << authorName << authorEmail << authorUri;
    authorString = authorName;
    if (!authorEmail.isEmpty()) authorString.append(QString(" <a href='mailto:%1'>e-mail</a>").arg(authorEmail));
    if (!authorUri.isEmpty())   authorString.append(QString(" <a href='%1'>page</a>").arg(authorUri));
  }

  webPanelAuthor_->setText(authorString);
  webPanelAuthorLabel_->setVisible(!authorString.isEmpty());
  webPanelAuthor_->setVisible(!authorString.isEmpty());
  webControlPanel_->setVisible(false);

  if (QApplication::mouseButtons() & Qt::MiddleButton) {
    slotNewsViewDoubleClicked(index);
  }
  if (!(QApplication::mouseButtons() & Qt::MiddleButton) || !embeddedBrowserOn_) {
    if (!showDescriptionNews_) {
      QString linkString = newsModel_->record(
            index.row()).field("link_href").value().toString();
      if (linkString.isEmpty())
        linkString = newsModel_->record(index.row()).field("link_alternate").value().toString();

      webControlPanel_->setVisible(true);
      webView_->load(QUrl(linkString.simplified()));
    } else {
      QString content = newsModel_->record(index.row()).field("content").value().toString();
      if (content.isEmpty()) {
        webView_->setHtml(
              newsModel_->record(index.row()).field("description").value().toString());
      } else {
        webView_->setHtml(content);
      }
    }
  }
}

void RSSListing::slotFeedsFilter()
{
  if (feedsFilterGroup_->checkedAction()->objectName() == "filterFeedsAll_") {
    if (feedsFilterAction != NULL) {
      feedsFilterAction->setChecked(true);
      setFeedsFilter(feedsFilterAction);
    } else {
      feedsFilterMenu_->popup(
            feedsToolBar_->mapToGlobal(QPoint(0, feedsToolBar_->height()-1)));
    }
  } else {
    filterFeedsAll_->setChecked(true);
    setFeedsFilter(filterFeedsAll_);
  }
}

void RSSListing::slotNewsFilter()
{
  if (newsFilterGroup_->checkedAction()->objectName() == "filterNewsAll_") {
    if (newsFilterAction != NULL) {
      newsFilterAction->setChecked(true);
      setNewsFilter(newsFilterAction);
    } else {
      newsFilterMenu_->popup(
            newsToolBar_->mapToGlobal(QPoint(0, newsToolBar_->height()-1)));
    }
  } else {
    filterNewsAll_->setChecked(true);
    setNewsFilter(filterNewsAll_);
  }
}

void RSSListing::slotTimerUpdateFeeds()
{
  if (autoUpdatefeeds_ && updateAllFeedsAct_->isEnabled()) {
    slotGetAllFeeds();
  }
}

void RSSListing::slotShowUpdateAppDlg()
{
  UpdateAppDialog *updateAppDialog = new UpdateAppDialog(langFileName_,
                                                         settings_, this);
  updateAppDialog->activateWindow();
  updateAppDialog->exec();
  delete updateAppDialog;
}

void RSSListing::appInstallTranslator()
{
  bool translatorLoad;
  qApp->removeTranslator(translator_);
#if defined(Q_OS_WIN) || defined(Q_OS_OS2)
  translatorLoad = translator_->load(QCoreApplication::applicationDirPath() +
                                     QString("/lang/quiterss_%1").arg(langFileName_));
#else
  translatorLoad = translator_->load(QString("/usr/share/quiterss/lang/quiterss_%1").
                                     arg(langFileName_));
#endif
  if (translatorLoad) qApp->installTranslator(translator_);
  else retranslateStrings();
}

void RSSListing::retranslateStrings() {
  webViewProgress_->setFormat(tr("Loading... (%p%)"));
  feedsTitleLabel_->setText(tr("Feeds"));
  webPanelTitleLabel_->setText(tr("Title:"));
  webPanelAuthorLabel_->setText(tr("Author:"));
  progressBar_->setFormat(tr("Update feeds... (%p%)"));

  QString str = statusUnread_->text();
  str = str.right(str.length() - str.indexOf(':') - 1).replace(" ", "");
  statusUnread_->setText(QString(tr(" Unread: %1 ")).arg(str));
  str = statusAll_->text();
  str = str.right(str.length() - str.indexOf(':') - 1).replace(" ", "");
  statusAll_->setText(QString(tr(" All: %1 ")).arg(str));

  str = traySystem->toolTip();
  QString info =
      "QuiteRSS\n" +
      QString(tr("New news: %1")).arg(str.section(": ", 1).section("\n", 0, 0)) +
      QString("\n") +
      QString(tr("Unread news: %1")).arg(str.section(": ", 2));
  traySystem->setToolTip(info);

  addFeedAct_->setText(tr("&Add..."));
  addFeedAct_->setToolTip(tr("Add new feed"));

  deleteFeedAct_->setText(tr("&Delete..."));
  deleteFeedAct_->setToolTip(tr("Delete selected feed"));

  importFeedsAct_->setText(tr("&Import feeds..."));
  importFeedsAct_->setToolTip(tr("Import feeds from OPML file"));

  exportFeedsAct_->setText(tr("&Export feeds..."));
  exportFeedsAct_->setToolTip(tr("Export feeds to OPML file"));

  exitAct_->setText(tr("E&xit"));

  if (autoLoadImages_) {
    autoLoadImagesToggle_->setText(tr("Load images"));
    autoLoadImagesToggle_->setToolTip(tr("Auto load images to news view"));
  } else {
    autoLoadImagesToggle_->setText(tr("No load images"));
    autoLoadImagesToggle_->setToolTip(tr("No load images to news view"));
  }

  updateFeedAct_->setText(tr("Update feed"));
  updateFeedAct_->setToolTip(tr("Update current feed"));

  updateAllFeedsAct_->setText(tr("Update all"));
  updateAllFeedsAct_->setToolTip(tr("Update all feeds"));

  markAllFeedRead_->setText(tr("Mark all feeds Read"));

  markNewsRead_->setText(tr("Mark Read/Unread"));
  markNewsRead_->setToolTip(tr("Mark current news read/unread"));

  markAllNewsRead_->setText(tr("Mark all news Read"));
  markAllNewsRead_->setToolTip(tr("Mark all news read"));


  setNewsFiltersAct_->setText(tr("News filters..."));
  setFilterNewsAct_->setText(tr("Filter news..."));

  optionsAct_->setText(tr("Options..."));
  optionsAct_->setToolTip(tr("Open options dialog"));

  feedsFilter_->setText(tr("Filter"));
  filterFeedsAll_->setText(tr("Show All"));
  filterFeedsNew_->setText(tr("Show New"));
  filterFeedsUnread_->setText(tr("Show Unread"));

  newsFilter_->setText( tr("Filter"));
  filterNewsAll_->setText(tr("Show All"));
  filterNewsNew_->setText(tr("Show New"));
  filterNewsUnread_->setText(tr("Show Unread"));
  filterNewsStar_->setText(tr("Show Star"));

  aboutAct_ ->setText(tr("About..."));
  aboutAct_->setToolTip(tr("Show 'About' dialog"));

  updateAppAct_->setText(tr("Check for updates..."));

  openInBrowserAct_->setText(tr("Open in browser"));
  openInExternalBrowserAct_->setText(tr("Open in external browser"));
  markStarAct_->setText(tr("Star"));
  markStarAct_->setToolTip(tr("Mark news star"));
  deleteNewsAct_->setText(tr("Delete"));
  deleteNewsAct_->setToolTip(tr("Delete selected news"));

  markFeedRead_->setText(tr("Mark Read"));
  markFeedRead_->setToolTip(tr("Mark feed read"));
  feedProperties_->setText(tr("Properties feed"));
  feedProperties_->setToolTip(tr("Properties feed"));

  fileMenu_->setTitle(tr("&File"));
  editMenu_->setTitle(tr("&Edit"));
  viewMenu_->setTitle(tr("&View"));
  feedMenu_->setTitle(tr("Fee&ds"));
  newsMenu_->setTitle(tr("&News"));
  toolsMenu_->setTitle(tr("&Tools"));
  helpMenu_->setTitle(tr("&Help"));

  toolBar_->setWindowTitle(tr("ToolBar"));
  toolBarMenu_->setTitle(tr("ToolBar"));
  toolBarStyleMenu_->setTitle(tr("Style"));
  toolBarStyleI_->setText(tr("Icon"));
  toolBarStyleT_->setText(tr("Text"));
  toolBarStyleTbI_->setText(tr("Text beside icon"));
  toolBarStyleTuI_->setText(tr("Text under icon"));
  toolBarToggle_->setText(tr("Show ToolBar"));

  toolBarIconSizeMenu_->setTitle(tr("Icon size"));
  toolBarIconBig_->setText(tr("Big"));
  toolBarIconNormal_->setText(tr("Normal"));
  toolBarIconSmall_->setText(tr("Small"));

  styleMenu_->setTitle(tr("Style application"));
  systemStyle_->setText(tr("System"));
  system2Style_->setText(tr("System2"));
  greenStyle_->setText(tr("Green"));
  orangeStyle_->setText(tr("Orange"));
  purpleStyle_->setText(tr("Purple"));
  pinkStyle_->setText(tr("Pink"));
  grayStyle_->setText(tr("Gray"));

  showWindowAct_->setText(tr("Show window"));

  feedKeyUpAct_->setText(tr("Previous feed"));
  feedKeyDownAct_->setText(tr("Next feed"));
  newsKeyUpAct_->setText(tr("Previous news"));
  newsKeyDownAct_->setText(tr("Next news"));

  switchFocusAct_->setText(tr("Switch focus between panels"));
  switchFocusAct_->setToolTip(
        tr("Switch focus between panels (tree feeds, list news, browser)"));

  visibleFeedsDockAct_->setText(tr("Show/hide tree feeds"));

  webView_->page()->action(QWebPage::OpenLink)->setText(tr("Open Link"));
  webView_->page()->action(QWebPage::OpenLinkInNewWindow)->setText(tr("Open in New Window"));
  webView_->page()->action(QWebPage::DownloadLinkToDisk)->setText(tr("Save Link..."));
  webView_->page()->action(QWebPage::CopyLinkToClipboard)->setText(tr("Copy Link"));
  webView_->page()->action(QWebPage::Copy)->setText(tr("Copy"));
  webView_->page()->action(QWebPage::Back)->setText(tr("Go Back"));
  webView_->page()->action(QWebPage::Forward)->setText(tr("Go Forward"));
  webView_->page()->action(QWebPage::Stop)->setText(tr("Stop"));
  webView_->page()->action(QWebPage::Reload)->setText(tr("Reload"));

  webHomePageAct_->setText(tr("Home"));
  webExternalBrowserAct_->setText(tr("Open in external browser"));

  QApplication::translate("QDialogButtonBox", "Cancel");
  QApplication::translate("QDialogButtonBox", "&Yes");
  QApplication::translate("QDialogButtonBox", "&No");

  QApplication::translate("QLineEdit", "&Undo");
  QApplication::translate("QLineEdit", "&Redo");
  QApplication::translate("QLineEdit", "Cu&t");
  QApplication::translate("QLineEdit", "&Copy");
  QApplication::translate("QLineEdit", "&Paste");
  QApplication::translate("QLineEdit", "Delete");
  QApplication::translate("QLineEdit", "Select All");

  QApplication::translate("QTextControl", "&Undo");
  QApplication::translate("QTextControl", "&Redo");
  QApplication::translate("QTextControl", "Cu&t");
  QApplication::translate("QTextControl", "&Copy");
  QApplication::translate("QTextControl", "&Paste");
  QApplication::translate("QTextControl", "Delete");
  QApplication::translate("QTextControl", "Select All");
  QApplication::translate("QTextControl", "Copy &Link Location");

  QApplication::translate("QAbstractSpinBox", "&Step up");
  QApplication::translate("QAbstractSpinBox", "Step &down");
  QApplication::translate("QAbstractSpinBox", "&Select All");

  QApplication::translate("QMultiInputContext", "Select IM");

  QApplication::translate("QWizard", "Cancel");
  QApplication::translate("QWizard", "< &Back");
  QApplication::translate("QWizard", "&Finish");
  QApplication::translate("QWizard", "&Next >");

  newsHeader_->retranslateStrings();
}

void RSSListing::setToolBarStyle(QAction *pAct)
{
  if (pAct->objectName() == "toolBarStyleI_") {
    toolBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  } else if (pAct->objectName() == "toolBarStyleT_") {
    toolBar_->setToolButtonStyle(Qt::ToolButtonTextOnly);
  } else if (pAct->objectName() == "toolBarStyleTbI_") {
    toolBar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  } else {
    toolBar_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  }
}

void RSSListing::setToolBarIconSize(QAction *pAct)
{
  if (pAct->objectName() == "toolBarIconBig_") {
    toolBar_->setIconSize(QSize(32, 32));
  } else if (pAct->objectName() == "toolBarIconSmall_") {
    toolBar_->setIconSize(QSize(16, 16));
  } else {
    toolBar_->setIconSize(QSize(24, 24));
  }
}

void RSSListing::showContextMenuToolBar(const QPoint &p)
{
  toolBarMenu_->popup(toolBar_->mapToGlobal(p));
}

void RSSListing::slotShowFeedPropertiesDlg()
{
  if (!feedsView_->currentIndex().isValid()){
    feedProperties_->setEnabled(false);
    return;
  }

  FeedPropertiesDialog *feedPropertiesDialog = new FeedPropertiesDialog(this);
  feedPropertiesDialog->restoreGeometry(settings_->value("feedProperties/geometry").toByteArray());

  QModelIndex index = feedsView_->currentIndex();

  QByteArray byteArray = feedsModel_->index(index.row(), feedsModel_->fieldIndex("image")).
      data().toByteArray();
  if (!byteArray.isNull()) {
    QPixmap icon;
    icon.loadFromData(QByteArray::fromBase64(byteArray));
    feedPropertiesDialog->setWindowIcon(icon);
  } else feedPropertiesDialog->setWindowIcon(QPixmap(":/images/feed"));
  QString str(feedPropertiesDialog->windowTitle() +
              " '" +
              feedsModel_->index(index.row(), 1).data().toString() +
              "'");
  feedPropertiesDialog->setWindowTitle(str);

  FEED_PROPERTIES properties;

  properties.general.text = feedsModel_->record(index.row()).field("text").value().toString();
  properties.general.title = feedsModel_->record(index.row()).field("title").value().toString();
  properties.general.url = feedsModel_->record(index.row()).field("xmlUrl").value().toString();
  properties.general.homepage = feedsModel_->record(index.row()).field("htmlUrl").value().toString();

  feedPropertiesDialog->setFeedProperties(properties);

  connect(feedPropertiesDialog, SIGNAL(signalLoadTitle(QUrl, QUrl)),
          faviconLoader, SLOT(requestUrl(QUrl, QUrl)));
  connect(feedPropertiesDialog, SIGNAL(startGetUrlTimer()),
          this, SIGNAL(startGetUrlTimer()));

  int result = feedPropertiesDialog->exec();
  settings_->setValue("feedProperties/geometry", feedPropertiesDialog->saveGeometry());
  if (result == QDialog::Rejected) {
    delete feedPropertiesDialog;
    return;
  }

  int id = feedsModel_->record(index.row()).field("id").value().toInt();

  properties = feedPropertiesDialog->getFeedProperties();
  delete feedPropertiesDialog;

  db_.exec(QString("update feeds set text = '%1' where id == '%2'").
           arg(properties.general.text).
           arg(id));
  db_.exec(QString("update feeds set xmlUrl = '%1' where id == '%2'").
           arg(properties.general.url).
           arg(id));

  feedsModel_->select();
  feedsView_->setCurrentIndex(index);

  byteArray = feedsModel_->index(index.row(), feedsModel_->fieldIndex("image")).
      data().toByteArray();
  if (!byteArray.isNull()) {
    QPixmap icon;
    icon.loadFromData(QByteArray::fromBase64(byteArray));
    newsIconTitle_->setPixmap(icon);
  } else newsIconTitle_->setPixmap(QPixmap(":/images/feed"));
  newsTextTitle_->setText(feedsModel_->index(index.row(), 1).data().toString());
}

void RSSListing::slotEditMenuAction()
{
  if (feedsView_->currentIndex().isValid()) feedProperties_->setEnabled(true);
  else  feedProperties_->setEnabled(false);
}

void RSSListing::refreshInfoTray()
{
  if (!showTrayIcon_) return;

  int newCount = 0;
  QSqlQuery q(db_);
  QString qStr = QString("select newCount from feeds");
  q.exec(qStr);
  while (q.next()) {
    newCount += q.value(0).toInt();
  }
  int unreadCount = 0;
  qStr = QString("select unread from feeds");
  q.exec(qStr);
  while (q.next()) {
    unreadCount += q.value(0).toInt();
  }

  QString info =
      "QuiteRSS\n" +
      QString(tr("New news: %1")).arg(newCount) +
      QString("\n") +
      QString(tr("Unread news: %1")).arg(unreadCount);
  traySystem->setToolTip(info);

  if (behaviorIconTray_ > 1) {
    if (behaviorIconTray_ == 3) newCount = unreadCount;
    if (newCount != 0) {
      QString newCountStr;
      QFont font;
      font.setFamily("Consolas");
      if (newCount > 99) {
        font.setBold(false);
        if (newCount < 1000) {
          font.setPixelSize(8);
          newCountStr = QString::number(newCount);
        } else {
          font.setPixelSize(11);
          newCountStr = "#";
        }
      } else {
        font.setBold(true);
        font.setPixelSize(11);
        newCountStr = QString::number(newCount);
      }

      QPixmap icon = QPixmap(":/images/countNew");
      QPainter trayPainter;
      trayPainter.begin(&icon);
      trayPainter.setFont(font);
      trayPainter.setPen(Qt::white);
      trayPainter.drawText(QRect(1, 0, 15, 16), Qt::AlignVCenter | Qt::AlignHCenter,
                           newCountStr);
      trayPainter.end();
      traySystem->setIcon(icon);
    } else traySystem->setIcon(QIcon(":/images/quiterss16"));
  }
}

void RSSListing::markAllFeedsRead(bool readOn)
{
  //! Помечаем все ленты прочитанными
  db_.transaction();
  QSqlQuery q(db_);
  if (!readOn) {
    q.exec("UPDATE news SET new=0");
    q.exec("UPDATE feeds SET newCount=0");
  } else {
    q.exec("UPDATE news SET new=0, read=2");
    q.exec("UPDATE feeds SET newCount=0, unread=0");
  }
  db_.commit();

  //! Перечитывание модели лент
  QModelIndex index = feedsView_->currentIndex();
  feedsModel_->select();
  feedsView_->setCurrentIndex(index);

  if (newsModel_->rowCount() != 0) {
    int currentRow = newsView_->currentIndex().row();

    setNewsFilter(newsFilterGroup_->checkedAction(), false);
    newsModel_->select();

    while (newsModel_->canFetchMore())
      newsModel_->fetchMore();

    int row = -1;
    for (int i = 0; i < newsModel_->rowCount(); i++) {
      if (newsModel_->index(i, newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt() ==
          feedsModel_->index(feedsView_->currentIndex().row(),
                             feedsModel_->fieldIndex("currentNews")).data().toInt()) {
        row = i;
        break;
      }
    }
    newsView_->setCurrentIndex(newsModel_->index(row, 6));
    if (currentRow != row)
      updateWebView(newsModel_->index(row, 0));
  }
  refreshInfoTray();
}

void RSSListing::slotWebTitleLinkClicked(QString urlStr)
{
  if (embeddedBrowserOn_) {
    webView_->history()->clear();
    webControlPanel_->setVisible(true);
    slotLinkClicked(QUrl(urlStr.simplified()));
  } else QDesktopServices::openUrl(QUrl(urlStr.simplified()));
}

void RSSListing::slotIconFeedLoad(const QString &strUrl, const QByteArray &byteArray)
{
  QSqlQuery q(db_);
  q.prepare("update feeds set image = ? where xmlUrl == ?");
  q.addBindValue(byteArray.toBase64());
  q.addBindValue(strUrl);
  q.exec();

  QModelIndex index = feedsView_->currentIndex();
  feedsModel_->select();
  feedsView_->setCurrentIndex(index);
}

void RSSListing::playSoundNewNews()
{
  if (!playSoundNewNews_ && soundNewNews_) {
#if defined(Q_OS_WIN) || defined(Q_OS_OS2)
    QSound::play(QCoreApplication::applicationDirPath() +
                 QString("/sound/notification.wav"));
#else
    QProcess::startDetached("play /usr/share/quiterss/sound/notification.wav");
#endif
    playSoundNewNews_ = true;
  }
}

void RSSListing::showNewsFiltersDlg()
{
  NewsFiltersDialog *newsFiltersDialog = new NewsFiltersDialog(this, settings_);

  QSqlQuery q(db_);
  QString qStr = QString("select text from feeds");
  q.exec(qStr);
  while (q.next()) {
    newsFiltersDialog->feedsList_ << q.value(0).toString();
  }

  newsFiltersDialog->exec();
  delete newsFiltersDialog;
}

void RSSListing::showFilterNewsDlg()
{
  QStringList feedsList;
  QSqlQuery q(db_);
  QString qStr = QString("select text from feeds");
  q.exec(qStr);
  while (q.next()) {
    feedsList << q.value(0).toString();
  }

  FilterRulesDialog *filterRulesDialog = new FilterRulesDialog(
        this, settings_, &feedsList);

  int result = filterRulesDialog->exec();
  if (result == QDialog::Rejected) {
    delete filterRulesDialog;
    return;
  }

  delete filterRulesDialog;
}

void RSSListing::slotUpdateAppChacking()
{
  updateAppDialog_ = new UpdateAppDialog(langFileName_, settings_, this, false);
  connect(updateAppDialog_, SIGNAL(signalNewVersion(bool)),
          this, SLOT(slotNewVersion(bool)), Qt::QueuedConnection);
}

void RSSListing::slotNewVersion(bool newVersion)
{
  delete updateAppDialog_;

  if (newVersion) {
    traySystem->showMessage("Check for updates",
                            "A new version of QuiteRSS...");
    connect(traySystem, SIGNAL(messageClicked()),
            this, SLOT(slotShowUpdateAppDlg()));
  }
}

/*! \brief Обработка клавиш Up/Down в дереве лент *****************************/
void RSSListing::slotFeedUpPressed()
{
  if (!feedsView_->currentIndex().isValid()) return;

  int row = feedsView_->currentIndex().row();
  if (row == 0) row = feedsModel_->rowCount()-1;
  else row--;
  feedsView_->setCurrentIndex(feedsModel_->index(row, 1));
  slotFeedsTreeClicked(feedsView_->currentIndex());
}

void RSSListing::slotFeedDownPressed()
{
  if (!feedsView_->currentIndex().isValid()) return;

  int row = feedsView_->currentIndex().row();
  if ((row+1) == feedsModel_->rowCount()) row = 0;
  else row++;
  feedsView_->setCurrentIndex(feedsModel_->index(row, 1));
  slotFeedsTreeClicked(feedsView_->currentIndex());
}

/*! \brief Обработка клавиш Up/Down в дереве новостей *************************/
void RSSListing::slotNewsUpPressed()
{
  if (!newsView_->currentIndex().isValid()) {
    if (newsModel_->rowCount() > 0) {
      newsView_->setCurrentIndex(newsModel_->index(0, 1));
      slotNewsViewClicked(newsView_->currentIndex());
    }
    return;
  }

  int row = newsView_->currentIndex().row();
  if (row == 0) return;
  else row--;
  newsView_->setCurrentIndex(newsModel_->index(row, 1));
  slotNewsViewClicked(newsView_->currentIndex());
}

void RSSListing::slotNewsDownPressed()
{
  if (!newsView_->currentIndex().isValid()) {
    if (newsModel_->rowCount() > 0) {
      newsView_->setCurrentIndex(newsModel_->index(0, 1));
      slotNewsViewClicked(newsView_->currentIndex());
    }
    return;
  }

  int row = newsView_->currentIndex().row();
  if ((row+1) == newsModel_->rowCount()) return;
  else row++;
  newsView_->setCurrentIndex(newsModel_->index(row, 1));
  slotNewsViewClicked(newsView_->currentIndex());
}

void RSSListing::feedsCleanUp(QString name)
{
  int cntT = 0;
  int cntNews = 0;

//  qDebug() << name;

  QSqlQuery q(db_);
  QString qStr = QString("SELECT count(id) FROM news WHERE feedId=='%1' AND deleted=0").
      arg(name);
  q.exec(qStr);
  if (q.next()) cntNews = q.value(0).toInt();

  qStr = QString("SELECT deleted, received, id, read, starred, published "
      "FROM news WHERE feedId=='%1'")
      .arg(name);
  q.exec(qStr);
  while (q.next()) {
    int id = q.value(2).toInt();
    int read = q.value(3).toInt();
    int starred = q.value(4).toInt();

    if ((neverUnreadCleanUp_ && (read == 0)) ||
        (neverStarCleanUp_ && (starred != 0)) ||
        q.value(0).toInt() != 0)
      continue;

    if ((cntT < (cntNews - maxNewsCleanUp_)) && newsCleanUpOn_) {
        qStr = QString("UPDATE news SET deleted=1 WHERE feedId=='%1' AND id='%2'").
            arg(name).arg(id);
//        qDebug() << "*01"  << id << q.value(5).toString()
//                 << q.value(1).toString() << cntNews
//                 << (cntNews - maxNewsCleanUp_);
        QSqlQuery qt(db_);
        qt.exec(qStr);
        cntT++;
        continue;
    }

    QDateTime dateTime = QDateTime::fromString(
          q.value(1).toString(),
          Qt::ISODate);
    if ((dateTime.daysTo(QDateTime::currentDateTime()) > maxDayCleanUp_) &&
        dayCleanUpOn_) {
        qStr = QString("UPDATE news SET deleted=1 WHERE feedId=='%1' AND id='%2'").
            arg(name).arg(id);
//        qDebug() << "*02"  << id << q.value(5).toString()
//                 << q.value(1).toString() << cntNews
//                 << (cntNews - maxNewsCleanUp_);
        QSqlQuery qt(db_);
        qt.exec(qStr);
        cntT++;
        continue;
    }

    if (readCleanUp_) {
      qStr = QString("UPDATE news SET deleted=1 WHERE feedId=='%1' AND read!=0 AND id='%2'").
          arg(name).arg(id);
      QSqlQuery qt(db_);
      qt.exec(qStr);
      cntT++;
    }
  }
}

void RSSListing::setStyleApp(QAction *pAct)
{
  QString fileString;
  if (pAct->objectName() == "systemStyle_") {
    fileString = ":/style/systemStyle";
  } else if (pAct->objectName() == "system2Style_") {
    fileString = ":/style/system2Style";
  } else if (pAct->objectName() == "orangeStyle_") {
    fileString = ":/style/orangeStyle";
  } else if (pAct->objectName() == "purpleStyle_") {
    fileString = ":/style/purpleStyle";
  } else if (pAct->objectName() == "pinkStyle_") {
    fileString = ":/style/pinkStyle";
  } else if (pAct->objectName() == "grayStyle_") {
    fileString = ":/style/grayStyle";
  } else {
    fileString = ":/style/greenStyle";
  }

  QFile file(fileString);
  file.open(QFile::ReadOnly);
  qApp->setStyleSheet(QLatin1String(file.readAll()));
}

void RSSListing::webHomePage()
{
  updateWebView(newsView_->currentIndex());
  webView_->history()->clear();
}

void RSSListing::openPageInExternalBrowser()
{
  QDesktopServices::openUrl(webView_->url());
}

void RSSListing::slotSwitchFocus()
{
  if (feedsView_->hasFocus()) {
    newsView_->setFocus();
  } else if (newsView_->hasFocus()) {
    webView_->setFocus();
  } else if (webView_->hasFocus()) {
    feedsView_->setFocus();
  }
}

void RSSListing::slotOpenNewsWebView()
{
  if (!newsView_->hasFocus()) return;
  slotNewsViewClicked(newsView_->currentIndex());
}
