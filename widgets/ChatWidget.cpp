// -*- Mode: C++ -*-
#include "ChatWidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>
#include <QScrollBar>
#include <QFont>
#include <QIntValidator>

ChatWidget::ChatWidget(QWidget *parent)
  : QWidget(parent)
  , m_protocol(nullptr)
{
  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(4, 4, 4, 4);
  mainLayout->setSpacing(4);

  // Chat history
  m_chatHistory = new QTextEdit(this);
  m_chatHistory->setReadOnly(true);
  m_chatHistory->setFont(QFont("Courier", 9));
  m_chatHistory->setMinimumHeight(100);
  mainLayout->addWidget(m_chatHistory, 1);

  // Station IDs row
  auto *idLayout = new QHBoxLayout;
  auto *myIdLabel = new QLabel(tr("Mon ID:"), this);
  m_myIdField = new QLineEdit("01", this);
  m_myIdField->setMaxLength(2);
  m_myIdField->setFixedWidth(35);
  m_myIdField->setValidator(new QIntValidator(1, 99, this));

  auto *targetLabel = new QLabel(tr("Dest:"), this);
  m_targetIdField = new QLineEdit("02", this);
  m_targetIdField->setMaxLength(2);
  m_targetIdField->setFixedWidth(35);
  m_targetIdField->setValidator(new QIntValidator(1, 99, this));

  idLayout->addWidget(myIdLabel);
  idLayout->addWidget(m_myIdField);
  idLayout->addSpacing(10);
  idLayout->addWidget(targetLabel);
  idLayout->addWidget(m_targetIdField);
  idLayout->addStretch();
  mainLayout->addLayout(idLayout);

  // Input row
  auto *inputLayout = new QHBoxLayout;
  m_inputField = new QLineEdit(this);
  m_inputField->setMaxLength(99);
  m_inputField->setPlaceholderText(tr("Message..."));
  m_sendButton = new QPushButton(tr("TX"), this);
  m_sendButton->setToolTip(tr("Envoi avec echo (point a point)"));
  m_broadcastButton = new QPushButton(tr("CQ"), this);
  m_broadcastButton->setToolTip(tr("Broadcast sans echo (/AR en fin)"));
  m_haltButton = new QPushButton(tr("Halt"), this);
  m_sendButton->setFixedWidth(40);
  m_broadcastButton->setFixedWidth(40);
  m_haltButton->setFixedWidth(45);
  inputLayout->addWidget(m_inputField, 1);
  inputLayout->addWidget(m_sendButton);
  inputLayout->addWidget(m_broadcastButton);
  inputLayout->addWidget(m_haltButton);
  mainLayout->addLayout(inputLayout);

  // Fragment label (hidden by default)
  m_fragmentLabel = new QLabel(this);
  m_fragmentLabel->setVisible(false);
  m_fragmentLabel->setFont(QFont("Courier", 8));
  m_fragmentLabel->setWordWrap(true);
  m_fragmentLabel->setStyleSheet("background: #1a1a2e; color: #e0e0e0; padding: 3px; border-radius: 3px;");
  mainLayout->addWidget(m_fragmentLabel);

  // Progress bar (hidden by default)
  m_txProgress = new QProgressBar(this);
  m_txProgress->setVisible(false);
  m_txProgress->setTextVisible(true);
  m_txProgress->setFixedHeight(16);
  mainLayout->addWidget(m_txProgress);

  // Status row
  auto *statusLayout = new QHBoxLayout;
  m_charCount = new QLabel("0/99", this);
  m_statusLabel = new QLabel(tr("Idle"), this);
  m_statusLabel->setStyleSheet("color: gray;");
  statusLayout->addWidget(m_charCount);
  statusLayout->addStretch();
  statusLayout->addWidget(m_statusLabel);
  mainLayout->addLayout(statusLayout);

  // Connections
  connect(m_sendButton, &QPushButton::clicked, this, &ChatWidget::onSendClicked);
  connect(m_broadcastButton, &QPushButton::clicked, this, &ChatWidget::onBroadcastClicked);
  connect(m_haltButton, &QPushButton::clicked, this, &ChatWidget::onHaltClicked);
  connect(m_inputField, &QLineEdit::textChanged, this, &ChatWidget::onTextChanged);
  connect(m_inputField, &QLineEdit::returnPressed, this, &ChatWidget::onSendClicked);
  connect(m_myIdField, &QLineEdit::textChanged, this, &ChatWidget::onMyIdChanged);
}

void ChatWidget::setProtocol(ChatProtocol *protocol)
{
  m_protocol = protocol;
  if (m_protocol) {
    m_protocol->setMyId(m_myIdField->text());
    connect(m_protocol, &ChatProtocol::messageReceived, this, &ChatWidget::onMessageReceived);
    connect(m_protocol, &ChatProtocol::messageSentOk, this, &ChatWidget::onMessageSentOk);
    connect(m_protocol, &ChatProtocol::fragmentProgress, this, &ChatWidget::onFragmentProgress);
    connect(m_protocol, &ChatProtocol::stateChanged, this, &ChatWidget::onStateChanged);
    connect(m_protocol, &ChatProtocol::statusMessage, this, &ChatWidget::onStatusMessage);
    connect(m_protocol, &ChatProtocol::directTxComplete, this, &ChatWidget::onDirectTxComplete);
    connect(m_protocol, &ChatProtocol::directFragmentStarted, this, &ChatWidget::onDirectFragmentStarted);
  }
}

QString ChatWidget::myId() const
{
  return m_myIdField->text().rightJustified(2, '0').left(2);
}

QString ChatWidget::targetId() const
{
  return m_targetIdField->text().rightJustified(2, '0').left(2);
}

QString ChatWidget::currentTimeStr() const
{
  return QDateTime::currentDateTimeUtc().toString("HH:mm");
}

void ChatWidget::appendChat(const QString &text, const QColor &color)
{
  m_chatHistory->setTextColor(color);
  m_chatHistory->append(text);
  QScrollBar *sb = m_chatHistory->verticalScrollBar();
  sb->setValue(sb->maximum());
}

// --- Slots ---

void ChatWidget::onSendClicked()
{
  QString text = m_inputField->text().trimmed();
  if (text.isEmpty()) return;

  QString target = targetId();
  m_pendingSentText = text;
  m_pendingTarget = target;
  m_pendingIsBroadcast = false;

  m_inputField->clear();
  emit directSendRequested(target, text);
}

void ChatWidget::onBroadcastClicked()
{
  QString text = m_inputField->text().trimmed();
  if (text.isEmpty()) return;

  QString target = targetId();
  m_pendingSentText = text;
  m_pendingTarget = target;
  m_pendingIsBroadcast = true;

  m_inputField->clear();
  emit directSendRequested(target, text);
}

void ChatWidget::onHaltClicked()
{
  emit haltRequested();
}

void ChatWidget::onTextChanged(const QString &text)
{
  m_charCount->setText(QString("%1/99").arg(text.length()));
}

void ChatWidget::onMyIdChanged(const QString &text)
{
  if (m_protocol) {
    m_protocol->setMyId(text);
  }
}

void ChatWidget::onMessageReceived(const QString &senderId, const QString &fullText)
{
  appendChat(currentTimeStr() + " " + senderId + ": " + fullText, Qt::white);
}

void ChatWidget::onMessageSentOk(const QString &targetId)
{
  if (!m_pendingSentText.isEmpty()) {
    QString prefix = m_pendingIsBroadcast ? "CQ>>" : ">>";
    appendChat(currentTimeStr() + " " + prefix + " [" + targetId + "] " + m_pendingSentText,
               m_pendingIsBroadcast ? QColor(255, 140, 0) : QColor(200, 0, 0));
    m_pendingSentText.clear();
  }
}

void ChatWidget::onFragmentProgress(int current, int total, bool isEcho)
{
  Q_UNUSED(isEcho)
  if (total > 0 && m_txProgress->isVisible()) {
    m_txProgress->setMaximum(total);
    m_txProgress->setValue(current);
  }
}

void ChatWidget::onStateChanged(ChatProtocol::State newState)
{
  switch (newState) {
  case ChatProtocol::Idle:
    m_statusLabel->setText(tr("Idle"));
    m_statusLabel->setStyleSheet("color: gray;");
    m_sendButton->setEnabled(true);
    m_broadcastButton->setEnabled(true);
    m_myIdField->setEnabled(true);
    m_targetIdField->setEnabled(true);
    m_txProgress->setVisible(false);
    m_fragmentLabel->setVisible(false);
    break;
  case ChatProtocol::SendingFragment:
    m_statusLabel->setText(tr("Envoi..."));
    m_statusLabel->setStyleSheet("color: orange;");
    m_sendButton->setEnabled(false);
    m_broadcastButton->setEnabled(false);
    m_myIdField->setEnabled(false);
    m_targetIdField->setEnabled(false);
    break;
  case ChatProtocol::WaitingEcho:
    m_statusLabel->setText(tr("Attente echo..."));
    m_statusLabel->setStyleSheet("color: #00b4d8;");
    m_sendButton->setEnabled(false);
    m_broadcastButton->setEnabled(false);
    break;
  case ChatProtocol::Broadcasting:
    m_statusLabel->setText(tr("Broadcast..."));
    m_statusLabel->setStyleSheet("color: #ff6d00;");
    m_sendButton->setEnabled(false);
    m_broadcastButton->setEnabled(false);
    m_myIdField->setEnabled(false);
    m_targetIdField->setEnabled(false);
    break;
  case ChatProtocol::DirectTx:
    m_statusLabel->setText(tr("Emission directe..."));
    m_statusLabel->setStyleSheet("color: #ff1744;");
    m_sendButton->setEnabled(false);
    m_broadcastButton->setEnabled(false);
    m_myIdField->setEnabled(false);
    m_targetIdField->setEnabled(false);
    m_txProgress->setVisible(true);
    m_fragmentLabel->setVisible(true);
    break;
  case ChatProtocol::EchoReady:
    m_statusLabel->setText(tr("Echo pret"));
    m_statusLabel->setStyleSheet("color: #ff9800;");
    m_sendButton->setEnabled(false);
    m_broadcastButton->setEnabled(false);
    break;
  case ChatProtocol::WaitingNext:
    m_statusLabel->setText(tr("Attente suite..."));
    m_statusLabel->setStyleSheet("color: #00b4d8;");
    m_sendButton->setEnabled(false);
    m_broadcastButton->setEnabled(false);
    break;
  case ChatProtocol::Complete:
    m_statusLabel->setText(tr("Termine!"));
    m_statusLabel->setStyleSheet("color: #00c853;");
    m_sendButton->setEnabled(true);
    m_broadcastButton->setEnabled(true);
    m_myIdField->setEnabled(true);
    m_targetIdField->setEnabled(true);
    m_txProgress->setVisible(false);
    m_fragmentLabel->setVisible(false);
    break;
  }
}

void ChatWidget::onStatusMessage(const QString &text)
{
  m_statusLabel->setText(text);
}

void ChatWidget::onDirectTxComplete()
{
  m_txProgress->setVisible(false);
  m_fragmentLabel->setVisible(false);
}

void ChatWidget::onDirectFragmentStarted(int current, int total,
                                          const QString &currentText,
                                          const QString &nextText)
{
  // Mettre à jour le label fragment
  QString label = QString("<b>TX %1/%2:</b> <span style='color:#ffab00'>%3</span>")
                    .arg(current).arg(total).arg(currentText.toHtmlEscaped());
  if (!nextText.isEmpty()) {
    label += QString("<br><b>Suivant:</b> <span style='color:#90a4ae'>%1</span>")
               .arg(nextText.toHtmlEscaped());
  } else {
    label += QString("<br><span style='color:#66bb6a'>Dernier fragment</span>");
  }
  m_fragmentLabel->setText(label);

  // Mettre à jour la barre de progression
  m_txProgress->setMaximum(total);
  m_txProgress->setValue(current);
  m_txProgress->setFormat(QString("%1/%2").arg(current).arg(total));
}
