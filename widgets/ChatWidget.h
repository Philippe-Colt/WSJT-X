// -*- Mode: C++ -*-
#ifndef CHATWIDGET_H
#define CHATWIDGET_H

#include <QWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include "ChatProtocol.h"

class ChatWidget : public QWidget
{
  Q_OBJECT

public:
  explicit ChatWidget(QWidget *parent = nullptr);

  void setProtocol(ChatProtocol *protocol);
  QString myId() const;
  QString targetId() const;

signals:
  void sendRequested(const QString &targetId, const QString &text);
  void broadcastRequested(const QString &targetId, const QString &text);
  void directSendRequested(const QString &targetId, const QString &text);
  void haltRequested();

public slots:
  void onMessageReceived(const QString &senderId, const QString &fullText);
  void onMessageSentOk(const QString &targetId);
  void onFragmentProgress(int current, int total, bool isEcho);
  void onStateChanged(ChatProtocol::State newState);
  void onStatusMessage(const QString &text);
  void onDirectTxComplete();
  void onDirectFragmentStarted(int current, int total,
                                const QString &currentText, const QString &nextText);

private slots:
  void onSendClicked();
  void onBroadcastClicked();
  void onHaltClicked();
  void onTextChanged(const QString &text);
  void onMyIdChanged(const QString &text);

private:
  void appendChat(const QString &text, const QColor &color);
  QString currentTimeStr() const;

  QTextEdit *m_chatHistory;
  QLineEdit *m_myIdField;
  QLineEdit *m_targetIdField;
  QLineEdit *m_inputField;
  QPushButton *m_sendButton;
  QPushButton *m_broadcastButton;
  QPushButton *m_haltButton;
  QLabel *m_charCount;
  QLabel *m_statusLabel;
  QLabel *m_fragmentLabel;   // Affiche le fragment en cours / suivant
  QProgressBar *m_txProgress;
  ChatProtocol *m_protocol;

  // Pending sent message (displayed only when fully transmitted)
  QString m_pendingSentText;
  QString m_pendingTarget;
  bool m_pendingIsBroadcast = false;
};

#endif // CHATWIDGET_H
