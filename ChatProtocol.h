// -*- Mode: C++ -*-
#ifndef CHATPROTOCOL_H
#define CHATPROTOCOL_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QElapsedTimer>

#include "commons.h"
#include "wsjtx_config.h"

// Fortran routines for FT8 encoding
extern "C" {
  void genft8_(char* msg, int* i3, int* n3, char* msgsent, char ft8msgbits[],
               int itone[], fortran_charlen_t, fortran_charlen_t);
  void gen_ft8wave_(int itone[], int* nsym, int* nsps, float* bt, float* fsample,
                    float* f0, float xjunk[], float wave[], int* icmplx, int* nwave);
}

//
// Protocole Chat HF par écho + broadcast
//
// Indicatifs sur 2 chiffres (01-99), mode point à point.
// Le premier fragment porte le header "XXYY " (expéditeur + destinataire).
// Les fragments suivants n'ont pas de header (13 chars complets).
//
// Mode Echo (point à point) :
//   Tx1  01→02 : "0102 HELLO WO"    (envoi fragment 1)
//   Tx2  02→01 : "0102 HELLO WO"    (écho = confirmé)
//   Tx3  01→02 : "RLD CMT CA V"     (envoi fragment 2)
//   Tx4  02→01 : "RLD CMT CA V"     (écho = confirmé)
//   Tx5  01→02 : "A 73"             (envoi fragment 3, dernier)
//   Tx6  02→01 : "A 73"             (écho = message complet)
//   Si écho ≠ envoyé → retransmission du même fragment.
//
// Mode Broadcast (en l'air) :
//   Tx1  01→02 : "0102 HELLO WO"    (fragment 1)
//   Tx2  01→02 : "RLD CMT CA V"     (fragment 2, continu)
//   Tx3  01→02 : "A 73       /AR"   (dernier, se termine par /AR)
//   Pas d'écho, envoi continu sur N slots.
//   Les 3 derniers chars du dernier fragment sont "/AR".
//   Côté récepteur, /AR détecte la fin du message.
//

class ChatProtocol : public QObject
{
  Q_OBJECT

public:
  enum State {
    Idle,
    // Expéditeur (mode écho)
    SendingFragment,    // Fragment prêt à émettre
    WaitingEcho,        // Fragment émis, attente de l'écho
    // Expéditeur (mode broadcast)
    Broadcasting,       // Envoi continu, pas d'écho attendu
    // Expéditeur (mode direct TX)
    DirectTx,           // Emission directe en cours (N trames FT8 concaténées)
    // Récepteur
    EchoReady,          // Fragment reçu, écho prêt à émettre
    WaitingNext,        // Écho émis, attente du prochain fragment
    // Commun
    Complete
  };
  Q_ENUM(State)

  explicit ChatProtocol(QObject *parent = nullptr);

  // Configuration
  void setMyId(const QString &id);
  QString myId() const { return m_myId; }

  // Envoi d'un message (rôle expéditeur, mode écho)
  void sendMessage(const QString &targetId, const QString &text);

  // Envoi broadcast (envoi continu sans écho, /AR en fin)
  void sendBroadcast(const QString &targetId, const QString &text);

  // Réception d'un décode free text (appelé par MainWindow)
  void processIncoming(const QString &freeText);

  // Texte à mettre dans Tx5 pour le prochain slot TX
  bool hasDataToSend() const;
  QString nextTxText();

  // Arrêt
  void haltTx();
  void notifyDirectTxComplete();

  // Emission directe : encode N trames FT8 dans foxcom_.wave[]
  void sendDirect(const QString &targetId, const QString &text, double txFreq);
  int prepareTxWaveform(const QStringList &fragments, double txFreq);
  void startDirectTxTracking();  // Démarre le suivi temps réel des fragments

  State state() const { return m_state; }
  int currentFragment() const { return m_fragIndex + 1; }
  int totalFragments() const { return m_fragments.size(); }

  // Constantes FT8 pour émission directe
  static constexpr int FT8_NSYM = 79;
  static constexpr int FT8_NSPS = 4 * 1920;           // 7680 samples/symbol at 48kHz
  static constexpr int SAMPLES_PER_FT8 = FT8_NSYM * FT8_NSPS; // 606720 (12.64s)
  static constexpr int SAMPLES_PER_PERIOD = 15 * 48000;        // 720000 (15.0s)

signals:
  void messageReceived(const QString &senderId, const QString &fullText);
  void messageSentOk(const QString &targetId);
  void stateChanged(ChatProtocol::State newState);
  void statusMessage(const QString &text);
  void fragmentProgress(int current, int total, bool isEcho);
  void directTxReady(int totalSymbols, int numFragments);
  void directTxComplete();
  // Emis quand un nouveau fragment commence à être transmis
  // currentText = fragment en cours, nextText = prochain fragment (vide si dernier)
  void directFragmentStarted(int current, int total,
                              const QString &currentText, const QString &nextText);

private:
  static constexpr int SLOT_SIZE = 13;
  static constexpr int HEADER_SIZE = 5;       // "XXYY " (4 digits + space)
  static constexpr int FIRST_PAYLOAD = SLOT_SIZE - HEADER_SIZE;  // 8 chars
  static constexpr int MAX_MESSAGE_LEN = 99;
  static constexpr int MAX_RETRIES = 5;
  static constexpr int TIMEOUT_MS = 90000;    // 90s global timeout

  static bool isValidFT8Char(QChar c);
  static QString filterFT8Text(const QString &text, int maxLen);

  // Fragmentation : découpe le message en fragments (le 1er avec header)
  QStringList fragmentMessage(const QString &senderId,
                              const QString &targetId,
                              const QString &text);

  // Fragmentation broadcast : comme fragmentMessage mais ajoute /AR au dernier
  QStringList fragmentBroadcast(const QString &senderId,
                                const QString &targetId,
                                const QString &text);

  // Détecte si un texte est un header "XXYY ..."
  static bool isHeader(const QString &text);
  static QString headerSender(const QString &text);
  static QString headerTarget(const QString &text);
  static QString headerPayload(const QString &text);

  // Détecte si un fragment se termine par /AR (fin de broadcast)
  static bool endsWithAR(const QString &text);
  static QString stripAR(const QString &text);

  void setState(State s);
  void reset();
  void deliverReceivedMessage();

  // Mon indicatif (2 chiffres)
  QString m_myId;

  // État global
  State m_state;

  // --- Expéditeur ---
  QString m_targetId;
  QStringList m_fragments;      // Tous les fragments à envoyer
  int m_fragIndex;              // Index du fragment courant
  QString m_lastSent;           // Dernier fragment envoyé (pour comparaison écho)
  int m_retryCount;
  bool m_broadcastMode;         // true = broadcast (pas d'écho)

  // --- Récepteur ---
  QString m_rxSenderId;         // Qui nous envoie
  QString m_echoText;           // Texte à renvoyer en écho
  QStringList m_rxPayloads;     // Payloads accumulés (pour réassemblage)

  // Timeout
  QTimer m_timeoutTimer;
  QTimer m_rxCompleteTimer;     // Timer pour détecter fin de message côté RX

  // Suivi émission directe
  QTimer m_directTxTracker;     // Timer périodique pour suivre l'avancement
  QElapsedTimer m_directTxElapsed; // Temps écoulé depuis début émission
  int m_directTxCurrentFrag;    // Fragment en cours (index 0-based)

private slots:
  void onTimeout();
  void onRxComplete();
  void onDirectTxTick();        // Tick du timer de suivi
};

#endif // CHATPROTOCOL_H
