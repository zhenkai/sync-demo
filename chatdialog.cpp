#include <QtGui>
#include "chatdialog.h"
#include "settingdialog.h"
#include <ctime>
#include <iostream>
#include <QTimer>
#include <QMetaType>
#include <QMessageBox>
#include <boost/random/random_device.hpp>
#include <boost/random/uniform_int_distribution.hpp>

#define BROADCAST_PREFIX_FOR_SYNC_DEMO "/ndn/broadcast/chronos"

static const int HELLO_INTERVAL = 90;  // seconds

ChatDialog::ChatDialog(QWidget *parent)
  : QDialog(parent), m_sock(NULL), m_lastMsgTime(0), m_reaped(false)
{
  // have to register this, otherwise
  // the signal-slot system won't recognize this type
  qRegisterMetaType<std::vector<Sync::MissingDataInfo> >("std::vector<Sync::MissingDataInfo>");
  qRegisterMetaType<size_t>("size_t");
  setupUi(this);
  m_session = time(NULL);
  boost::random::random_device rng;
  boost::random::uniform_int_distribution<> uniform(1, 29000);
  m_randomizedInterval = HELLO_INTERVAL * 1000 + uniform(rng);

  readSettings();

  updateLabels();

  lineEdit->setFocusPolicy(Qt::StrongFocus);
  m_scene = new DigestTreeScene(this);

  treeViewer->setScene(m_scene);
  m_scene->plot("Empty");
  QRectF rect = m_scene->itemsBoundingRect();
  m_scene->setSceneRect(rect);

  listView->setStyleSheet("QListView { alternate-background-color: white; background: #F0F0F0; color: darkGreen; font: bold large; }");
  listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
  listView->setDragDropMode(QAbstractItemView::NoDragDrop);
  listView->setSelectionMode(QAbstractItemView::NoSelection);
  m_rosterModel = new QStringListModel(this);
  listView->setModel(m_rosterModel);

  createActions();
  createTrayIcon();
  m_timer = new QTimer(this);
  connect(lineEdit, SIGNAL(returnPressed()), this, SLOT(returnPressed()));
  connect(setButton, SIGNAL(pressed()), this, SLOT(buttonPressed()));
  connect(treeButton, SIGNAL(pressed()), this, SLOT(treeButtonPressed()));
  connect(this, SIGNAL(dataReceived(QString, const char *, size_t, bool)), this, SLOT(processData(QString, const char *, size_t, bool)));
  connect(this, SIGNAL(treeUpdated(const std::vector<Sync::MissingDataInfo>)), this, SLOT(processTreeUpdate(const std::vector<Sync::MissingDataInfo>)));
  connect(m_timer, SIGNAL(timeout()), this, SLOT(replot()));
  connect(m_scene, SIGNAL(replot()), this, SLOT(replot()));
  connect(trayIcon, SIGNAL(messageClicked()), this, SLOT(showNormal()));
  connect(trayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)), this, SLOT(iconActivated(QSystemTrayIcon::ActivationReason)));
  connect(m_scene, SIGNAL(rosterChanged(QStringList)), this, SLOT(updateRosterList(QStringList)));

  // create sync socket
  if(!m_user.getChatroom().isEmpty()) {
    std::string syncPrefix = BROADCAST_PREFIX_FOR_SYNC_DEMO;
    syncPrefix += "/";
    syncPrefix += m_user.getChatroom().toStdString();
    try 
    {
      m_sock = new Sync::SyncAppSocket(syncPrefix, bind(&ChatDialog::processTreeUpdateWrapper, this, _1, _2), bind(&ChatDialog::processRemoveWrapper, this, _1));
      sendJoin();
      m_timer->start(FRESHNESS * 2000);
    }
    catch (Sync::CcnxOperationException ex)
    {
      QMessageBox::critical(this, tr("Chronos"), tr("Canno connect to ccnd.\n Have you started your ccnd?"), QMessageBox::Ok);
      std::exit(1);
    }
  }
  
}

ChatDialog::~ChatDialog()
{
  if (m_sock != NULL) 
  {
    SyncDemo::ChatMessage msg;
    formControlMessage(msg, SyncDemo::ChatMessage::LEAVE);
    sendMsg(msg);
    usleep(500000);
    m_sock->remove(m_user.getPrefix().toStdString());
    usleep(5000);
#ifdef __DEBUG
    std::cout << "Sync REMOVE signal sent" << std::endl;
#endif
    delete m_sock;
    m_sock = NULL;
  }
}

void
ChatDialog::replot()
{
  boost::recursive_mutex::scoped_lock lock(m_sceneMutex);
  m_scene->plot(m_sock->getRootDigest().c_str());
  fitView();
}

void
ChatDialog::updateRosterList(QStringList staleUserList)
{
  boost::recursive_mutex::scoped_lock lock(m_sceneMutex);
  QStringList rosterList = m_scene->getRosterList();
  m_rosterModel->setStringList(rosterList);
  QString user;
  QStringListIterator it(staleUserList);
  while(it.hasNext())
  {
    std::string nick = it.next().toStdString();
    if (nick.empty())
      continue;

    SyncDemo::ChatMessage msg;
    formControlMessage(msg, SyncDemo::ChatMessage::LEAVE);
    msg.set_from(nick);
    appendMessage(msg);
  }
}

void 
ChatDialog::setVisible(bool visible)
{
  minimizeAction->setEnabled(visible);
  maximizeAction->setEnabled(!isMaximized());
  restoreAction->setEnabled(isMaximized() || !visible);
  QDialog::setVisible(visible);
}

void 
ChatDialog::closeEvent(QCloseEvent *e)
{
  if (trayIcon->isVisible() && !m_minimaniho)
  {
    QMessageBox::information(this, tr("Chronos"),
			     tr("The program will keep running in the "
				"system tray. To terminate the program"
				"choose <b>Quit</b> in the context memu"
				"of the system tray entry."));
    hide();
    e->ignore();
    m_minimaniho = true;
    writeSettings();
  }
}

void 
ChatDialog::changeEvent(QEvent *e)
{
  switch(e->type()) 
  {
  case QEvent::ActivationChange:
    if (isActiveWindow()) 
    {
      trayIcon->setIcon(QIcon(":/images/icon_small.png"));
    }
    break;
  default:
    break;
  }
}

void
ChatDialog::appendMessage(const SyncDemo::ChatMessage msg) 
{
  boost::recursive_mutex::scoped_lock lock(m_msgMutex);

  if (msg.type() == SyncDemo::ChatMessage::CHAT) 
  {

    if (!msg.has_data())
    {
      return;
    }

    if (msg.from().empty() || msg.data().empty())
    {
      return;
    }

    if (!msg.has_timestamp())
    {
      return;
    }

    QTextCharFormat nickFormat;
    nickFormat.setForeground(Qt::darkGreen);
    nickFormat.setFontWeight(QFont::Bold);
    nickFormat.setFontUnderline(true);
    nickFormat.setUnderlineColor(Qt::gray);

    QTextCursor cursor(textEdit->textCursor());
    cursor.movePosition(QTextCursor::End);
    QTextTableFormat tableFormat;
    tableFormat.setBorder(0);
    QTextTable *table = cursor.insertTable(1, 2, tableFormat);
    QString from = QString("%1 ").arg(msg.from().c_str());
    QTextTableCell fromCell = table->cellAt(0, 0);
    fromCell.setFormat(nickFormat);
    fromCell.firstCursorPosition().insertText(from);

    time_t timestamp = msg.timestamp();
    printTimeInCell(table, timestamp);
    
    QTextCursor nextCursor(textEdit->textCursor());
    nextCursor.movePosition(QTextCursor::End);
    table = nextCursor.insertTable(1, 1, tableFormat);
    table->cellAt(0, 0).firstCursorPosition().insertText(msg.data().c_str());
    showMessage(from, msg.data().c_str());
  }

  if (msg.type() == SyncDemo::ChatMessage::JOIN || msg.type() == SyncDemo::ChatMessage::LEAVE)
  {
    QTextCharFormat nickFormat;
    nickFormat.setForeground(Qt::gray);
    nickFormat.setFontWeight(QFont::Bold);
    nickFormat.setFontUnderline(true);
    nickFormat.setUnderlineColor(Qt::gray);

    QTextCursor cursor(textEdit->textCursor());
    cursor.movePosition(QTextCursor::End);
    QTextTableFormat tableFormat;
    tableFormat.setBorder(0);
    QTextTable *table = cursor.insertTable(1, 2, tableFormat);
    QString action;
    if (msg.type() == SyncDemo::ChatMessage::JOIN)
    {
      action = "enters room";
    }
    else
    {
      action = "leaves room";
    }

    QString from = QString("%1 %2  ").arg(msg.from().c_str()).arg(action);
    QTextTableCell fromCell = table->cellAt(0, 0);
    fromCell.setFormat(nickFormat);
    fromCell.firstCursorPosition().insertText(from);

    time_t timestamp = msg.timestamp();
    printTimeInCell(table, timestamp);
  }

  QScrollBar *bar = textEdit->verticalScrollBar();
  bar->setValue(bar->maximum());
}

void 
ChatDialog::printTimeInCell(QTextTable *table, time_t timestamp)
{
  QTextCharFormat timeFormat;
  timeFormat.setForeground(Qt::gray);
  timeFormat.setFontUnderline(true);
  timeFormat.setUnderlineColor(Qt::gray);
  QTextTableCell timeCell = table->cellAt(0, 1);
  timeCell.setFormat(timeFormat);
  timeCell.firstCursorPosition().insertText(formatTime(timestamp));
}

QString
ChatDialog::formatTime(time_t timestamp)
{
  struct tm *tm_time = localtime(&timestamp);
  int hour = tm_time->tm_hour;
  QString amOrPM;
  if (hour > 12)
  {
    hour -= 12;
    amOrPM = "PM";
  }
  else
  {
    amOrPM = "AM";
    if (hour == 0)
    {
      hour = 12;
    }
  }

  char textTime[12];
  sprintf(textTime, "%d:%02d:%02d %s", hour, tm_time->tm_min, tm_time->tm_sec, amOrPM.toStdString().c_str());
  return QString(textTime);
}

void
ChatDialog::processTreeUpdateWrapper(const std::vector<Sync::MissingDataInfo> v, Sync::SyncAppSocket *sock)
{
  emit treeUpdated(v);
#ifdef __DEBUG
  std::cout << "<<< Tree update signal emitted" << std::endl;
#endif
}

void
ChatDialog::processTreeUpdate(const std::vector<Sync::MissingDataInfo> v)
{
#ifdef __DEBUG
  std::cout << "<<< processing Tree Update" << std::endl;
#endif
  if (v.empty())
  {
    return;
  }

  // reflect the changes on digest tree
  {
    boost::recursive_mutex::scoped_lock lock(m_sceneMutex);
    m_scene->processUpdate(v, m_sock->getRootDigest().c_str());
  }

  int n = v.size();
  int totalMissingPackets = 0;

  for (int i = 0; i < n; i++) 
  {
    totalMissingPackets += v[i].high.getSeq() - v[i].low.getSeq() + 1;
  }
  
  for (int i = 0; i < n; i++) 
  {
    if (!m_reaped)
    {
      m_reaperMap.insert(v[i].prefix, false);
    }

    if (totalMissingPackets < 4)
    {
      for (Sync::SeqNo seq = v[i].low; seq <= v[i].high; ++seq)
      {
        m_sock->fetchRaw(v[i].prefix, seq, bind(&ChatDialog::processDataWrapper, this, _1, _2, _3), 2);
#ifdef __DEBUG
        std::cout << "<<< Fetching " << v[i].prefix << "/" <<seq.getSession() <<"/" << seq.getSeq() << std::endl;
#endif
      }
    }
    else
    {
        m_sock->fetchRaw(v[i].prefix, v[i].high, bind(&ChatDialog::processDataNoShowWrapper, this, _1, _2, _3), 2);
    }
  }


  if (!m_reaped)
  {
    std::cout << "Totol zombie count " << m_reaperMap.size() << std::endl;
  }

  m_reaped = true;

  // adjust the view
  fitView();

}

void
ChatDialog::processDataWrapper(std::string name, const char *buf, size_t len)
{
  emit dataReceived(name.c_str(), buf, len, true);
  m_reaperMap.remove(name.c_str());
#ifdef __DEBUG
  std::cout <<"<<< " << name << " fetched" << std::endl;
#endif
}

void
ChatDialog::processDataNoShowWrapper(std::string name, const char *buf, size_t len)
{
  emit dataReceived(name.c_str(), buf, len, false);
  m_reaperMap.remove(name.c_str());
  std::cout <<"<<< " << name << " fetched" << std::endl;
}

void
ChatDialog::processData(QString name, const char *buf, size_t len, bool show)
{
  SyncDemo::ChatMessage msg;
  if (!msg.ParseFromArray(buf, len)) 
  {
    std::cerr << "Errrrr.. Can not parse msg with name: " << name.toStdString() << ". what is happening?" << std::endl;
  }

  // display msg received from network
  // we have to do so; this function is called by ccnd thread
  // so if we call appendMsg directly
  // Qt crash as "QObject: Cannot create children for a parent that is in a different thread"
  // the "cannonical" way to is use signal-slot
  if (show)
  {
    appendMessage(msg);
  }
  
  // update the tree view
  std::string stdStrName = name.toStdString();
  std::string stdStrNameWithoutSeq = stdStrName.substr(0, stdStrName.find_last_of('/'));
  std::string prefix = stdStrNameWithoutSeq.substr(0, stdStrNameWithoutSeq.find_last_of('/'));
#ifdef __DEBUG
  std::cout <<"<<< updating scene for" << prefix << ": " << msg.from()  << std::endl;
#endif
  if (msg.type() == SyncDemo::ChatMessage::LEAVE)
  {
    processRemove(prefix.c_str());
  }
  else
  {
    boost::recursive_mutex::scoped_lock lock(m_sceneMutex);
    m_scene->msgReceived(prefix.c_str(), msg.from().c_str());
  }
  fitView();
}

void
ChatDialog::processRemoveWrapper(std::string prefix)
{
#ifdef __DEBUG
  std::cout << "Sync REMOVE signal received for prefix: " << prefix << std::endl;
#endif
  //emit removeReceived(prefix.c_str());
}

void
ChatDialog::processRemove(QString prefix)
{
#ifdef __DEBUG
  std::cout << "<<< remove node for prefix" << prefix.toStdString() << std::endl;
#endif
  bool removed = m_scene->removeNode(prefix);
  if (removed)
  {
    boost::recursive_mutex::scoped_lock lock(m_sceneMutex);
    m_scene->plot(m_sock->getRootDigest().c_str());
  }
}

void
ChatDialog::formChatMessage(const QString &text, SyncDemo::ChatMessage &msg) {
  msg.set_from(m_user.getNick().toStdString());
  msg.set_to(m_user.getChatroom().toStdString());
  msg.set_data(text.toStdString());
  time_t seconds = time(NULL);
  msg.set_timestamp(seconds);
  msg.set_type(SyncDemo::ChatMessage::CHAT);
}

void 
ChatDialog::formControlMessage(SyncDemo::ChatMessage &msg, SyncDemo::ChatMessage::ChatMessageType type)
{
  msg.set_from(m_user.getNick().toStdString());
  msg.set_to(m_user.getChatroom().toStdString());
  time_t seconds = time(NULL);
  msg.set_timestamp(seconds);
  msg.set_type(type);
}

static std::string chars("qwertyuiopasdfghjklzxcvbnmQWERTYUIOPASDFGHJKLZXCVBNM0123456789");

QString
ChatDialog::getRandomString()
{
  std::string randStr;
  boost::random::random_device rng;
  boost::random::uniform_int_distribution<> index_dist(0, chars.size() - 1);
  for (int i = 0; i < 10; i ++)
  {
    randStr += chars[index_dist(rng)];
  }
  return randStr.c_str();
}

bool
ChatDialog::readSettings()
{
  QSettings s(ORGANIZATION, APPLICATION);
  QString nick = s.value("nick", "").toString();
  QString chatroom = s.value("chatroom", "").toString();
  QString originPrefix = s.value("originPrefix", "").toString();
  m_minimaniho = s.value("minimaniho", false).toBool();
  if (nick == "" || chatroom == "" || originPrefix == "") {
    QTimer::singleShot(500, this, SLOT(buttonPressed()));
    return false;
  }
  else {
    m_user.setNick(nick);
    m_user.setChatroom(chatroom);
    m_user.setOriginPrefix(originPrefix);
    m_user.setPrefix(originPrefix + "/" + chatroom + "/" + getRandomString());
    return true;
  }

//  QTimer::singleShot(500, this, SLOT(buttonPressed()));
 // return false;
}

void 
ChatDialog::writeSettings()
{
  QSettings s(ORGANIZATION, APPLICATION);
  s.setValue("nick", m_user.getNick());
  s.setValue("chatroom", m_user.getChatroom());
  s.setValue("originPrefix", m_user.getOriginPrefix());
  s.setValue("minimaniho", m_minimaniho);
}

void
ChatDialog::updateLabels()
{
  QString settingDisp = QString("Chatroom: %1").arg(m_user.getChatroom());
  infoLabel->setStyleSheet("QLabel {color: #630; font-size: 16px; font: bold \"Verdana\";}");
  infoLabel->setText(settingDisp);
  //QString prefixDisp = QString("<Prefix: %1>").arg(m_user.getPrefix());
  //prefixLabel->setText(prefixDisp);
}

void 
ChatDialog::returnPressed()
{
  QString text = lineEdit->text();
  if (text.isEmpty())
    return;
 
  lineEdit->clear();

  SyncDemo::ChatMessage msg;
  formChatMessage(text, msg);
  
  appendMessage(msg);

  sendMsg(msg);

  fitView();
}

void 
ChatDialog::sendMsg(SyncDemo::ChatMessage &msg)
{
  // send msg
  size_t size = msg.ByteSize();
  char *buf = new char[size];
  msg.SerializeToArray(buf, size);
  if (!msg.IsInitialized()) 
  {
    std::cerr << "Errrrr.. msg was not probally initialized "<<__FILE__ <<":"<<__LINE__<<". what is happening?" << std::endl;
    abort();
  }
  m_sock->publishRaw(m_user.getPrefix().toStdString(), m_session, buf, size, FRESHNESS);

  delete buf;

  m_lastMsgTime = time(NULL);

  int nextSequence = m_sock->getNextSeq(m_user.getPrefix().toStdString(), m_session);
  Sync::MissingDataInfo mdi = {m_user.getPrefix().toStdString(), Sync::SeqNo(0), Sync::SeqNo(nextSequence - 1)};
  std::vector<Sync::MissingDataInfo> v;
  v.push_back(mdi);
  {
    boost::recursive_mutex::scoped_lock lock(m_sceneMutex);
    m_scene->processUpdate(v, m_sock->getRootDigest().c_str());
    m_scene->msgReceived(m_user.getPrefix(), m_user.getNick());
  }
}

void 
ChatDialog::sendJoin()
{
  SyncDemo::ChatMessage msg;
  formControlMessage(msg, SyncDemo::ChatMessage::JOIN);
  sendMsg(msg);
  QTimer::singleShot(m_randomizedInterval, this, SLOT(sendHello()));
  QTimer::singleShot(2000, this, SLOT(kill()));
}

void
ChatDialog::kill()
{
  QMapIterator<std::string, bool> it(m_reaperMap);
  while(it.hasNext())
  {
    it.next();
    std::string prefix = it.key();
    m_sock->remove(prefix); 
    std::cout << "Cleaned prefix: " << prefix << std::endl;
    QTimer::singleShot(2000, this, SLOT(kill()));
    m_reaperMap.remove(prefix);
    std::cout << "zombie count " << m_reaperMap.size() << std::endl;
    break;
  }
}

void 
ChatDialog::sendHello()
{
  time_t now = time(NULL);
  int elapsed = now - m_lastMsgTime;
  if (elapsed >= m_randomizedInterval / 1000)
  {
    SyncDemo::ChatMessage msg;
    formControlMessage(msg, SyncDemo::ChatMessage::HELLO);
    sendMsg(msg);
    QTimer::singleShot(m_randomizedInterval, this, SLOT(sendHello()));
  }
  else
  {
    QTimer::singleShot((m_randomizedInterval - elapsed * 1000), this, SLOT(sendHello()));
  }
}

void
ChatDialog::buttonPressed()
{
  SettingDialog dialog(this, m_user.getNick(), m_user.getChatroom(), m_user.getOriginPrefix());
  connect(&dialog, SIGNAL(updated(QString, QString, QString)), this, SLOT(settingUpdated(QString, QString, QString)));
  dialog.exec();
  QTimer::singleShot(100, this, SLOT(checkSetting()));
}

void ChatDialog::treeButtonPressed()
{
  if (treeViewer->isVisible())
  {
    treeViewer->hide();
    treeButton->setText("Show Sync Tree");
  }
  else
  {
    treeViewer->show();
    treeButton->setText("Hide Sync Tree");
  }

  fitView();
}

void
ChatDialog::checkSetting()
{
  if (m_user.getOriginPrefix().isEmpty() || m_user.getNick().isEmpty() || m_user.getChatroom().isEmpty())
  {
    buttonPressed();
  }
}

void
ChatDialog::settingUpdated(QString nick, QString chatroom, QString originPrefix)
{
  QString randString = getRandomString();
  bool needWrite = false;
  bool needFresh = false;
  if (!nick.isEmpty() && nick != m_user.getNick()) {
    m_user.setNick(nick);
    needWrite = true;
  }
  if (!originPrefix.isEmpty() && originPrefix != m_user.getOriginPrefix()) {
    m_user.setOriginPrefix(originPrefix);
    m_user.setPrefix(originPrefix + "/" + m_user.getChatroom() + "/" + randString);
    needWrite = true;
    needFresh = true;
  }
  if (!chatroom.isEmpty() && chatroom != m_user.getChatroom()) {
    m_user.setChatroom(chatroom);
    m_user.setPrefix(m_user.getOriginPrefix() + "/" + chatroom + "/" + randString);
    needWrite = true;
    needFresh = true;
  }

  if (needFresh)
  {

    {
      boost::recursive_mutex::scoped_lock lock(m_sceneMutex);
      m_scene->clearAll();
      m_scene->plot("Empty");
    }

    textEdit->clear();

    // TODO: perhaps need to do a lot. e.g. use a new SyncAppSokcet
    if (m_sock != NULL) 
    {
      delete m_sock;
      m_sock = NULL;
    }
    std::string syncPrefix = BROADCAST_PREFIX_FOR_SYNC_DEMO;
    syncPrefix += "/";
    syncPrefix += m_user.getChatroom().toStdString();
    try
    {
      m_sock = new Sync::SyncAppSocket(syncPrefix, bind(&ChatDialog::processTreeUpdateWrapper, this, _1, _2), bind(&ChatDialog::processRemoveWrapper, this, _1));
      sendJoin();
      m_timer->start(FRESHNESS * 2000);
    }
    catch (Sync::CcnxOperationException ex)
    {
      QMessageBox::critical(this, tr("Chronos"), tr("Canno connect to ccnd.\n Have you started your ccnd?"), QMessageBox::Ok);
      std::exit(1);
    }

    fitView();
    
  }

  if (needWrite) {
    writeSettings();
    updateLabels();
  }
}

void
ChatDialog::iconActivated(QSystemTrayIcon::ActivationReason reason)
{
  switch (reason)
  {
  case QSystemTrayIcon::Trigger:
  case QSystemTrayIcon::DoubleClick:
    break;
  case QSystemTrayIcon::MiddleClick:
    // showMessage();
    break;
  default:;
  }
}

void
ChatDialog::showMessage(QString from, QString data)
{
  //  std::cout <<"Showing Message: " << from.toStdString() << ": " << data.toStdString() << std::endl;
  if (!isActiveWindow()) 
  {
    trayIcon->showMessage(QString("Chatroom %1 has a new message").arg(m_user.getChatroom()), QString("<%1>: %2").arg(from).arg(data), QSystemTrayIcon::Information, 20000);
    trayIcon->setIcon(QIcon(":/images/note.png"));
  }
}

void
ChatDialog::messageClicked()
{
  this->showMaximized();
}

void
ChatDialog::createActions()
{
  minimizeAction = new QAction(tr("Mi&nimize"), this);
  connect(minimizeAction, SIGNAL(triggered()), this, SLOT(hide()));
  
  maximizeAction = new QAction(tr("Ma&ximize"), this);
  connect(maximizeAction, SIGNAL(triggered()), this, SLOT(showMaximized()));
  
  restoreAction = new QAction(tr("&Restore"), this);
  connect(restoreAction, SIGNAL(triggered()), this, SLOT(showNormal()));
  
  quitAction = new QAction(tr("Quit"), this);
  connect(quitAction, SIGNAL(triggered()), qApp, SLOT(quit()));
}

void
ChatDialog::createTrayIcon()
{
  trayIconMenu = new QMenu(this);
  trayIconMenu->addAction(minimizeAction);
  trayIconMenu->addAction(maximizeAction);
  trayIconMenu->addAction(restoreAction);
  trayIconMenu->addSeparator();
  trayIconMenu->addAction(quitAction);

  trayIcon = new QSystemTrayIcon(this);
  trayIcon->setContextMenu(trayIconMenu);

  QIcon icon(":/images/icon_small.png");
  trayIcon->setIcon(icon);
  setWindowIcon(icon);
  trayIcon->setToolTip("Chronos System Tray Icon");
  trayIcon->setVisible(true);
}

void 
ChatDialog::resizeEvent(QResizeEvent *e)
{
  fitView();
}

void 
ChatDialog::showEvent(QShowEvent *e)
{
  fitView();
}

void
ChatDialog::fitView()
{
  boost::recursive_mutex::scoped_lock lock(m_sceneMutex);
  QRectF rect = m_scene->itemsBoundingRect();
  m_scene->setSceneRect(rect);
  treeViewer->fitInView(m_scene->itemsBoundingRect(), Qt::KeepAspectRatio);
}
