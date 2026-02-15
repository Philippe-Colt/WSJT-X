// -*- Mode: C++ -*-
#include "ChatProtocol.h"
#include <QDebug>
#include <cstring>

// --- Helpers FT8 ---

bool ChatProtocol::isValidFT8Char(QChar c)
{
  if (c >= 'A' && c <= 'Z') return true;
  if (c >= '0' && c <= '9') return true;
  if (c == ' ' || c == '+' || c == '-' || c == '.' || c == '/' || c == '?') return true;
  return false;
}

QString ChatProtocol::filterFT8Text(const QString &text, int maxLen)
{
  QString result;
  QString upper = text.toUpper();
  for (int i = 0; i < upper.size() && result.size() < maxLen; ++i) {
    QChar c = upper.at(i);
    if (isValidFT8Char(c)) {
      result.append(c);
    }
  }
  return result;
}

// --- Header detection ---

bool ChatProtocol::isHeader(const QString &text)
{
  // Header = 4 digits + space + payload : "0102 HELLO WO"
  if (text.size() < HEADER_SIZE) return false;
  for (int i = 0; i < 4; ++i) {
    if (!text.at(i).isDigit()) return false;
  }
  return text.at(4) == ' ';
}

QString ChatProtocol::headerSender(const QString &text)
{
  if (!isHeader(text)) return QString();
  return text.left(2);
}

QString ChatProtocol::headerTarget(const QString &text)
{
  if (!isHeader(text)) return QString();
  return text.mid(2, 2);
}

QString ChatProtocol::headerPayload(const QString &text)
{
  if (!isHeader(text)) return QString();
  return text.mid(HEADER_SIZE);
}

// --- /AR detection ---

bool ChatProtocol::endsWithAR(const QString &text)
{
  QString trimmed = text.trimmed();
  return trimmed.endsWith("/AR");
}

QString ChatProtocol::stripAR(const QString &text)
{
  QString trimmed = text.trimmed();
  if (trimmed.endsWith("/AR")) {
    trimmed.chop(3);  // Remove "/AR"
  }
  return trimmed.trimmed();
}

// --- Fragmentation ---

QStringList ChatProtocol::fragmentMessage(const QString &senderId,
                                          const QString &targetId,
                                          const QString &text)
{
  QStringList result;
  QString clean = filterFT8Text(text, MAX_MESSAGE_LEN);
  if (clean.isEmpty()) return result;

  int pos = 0;
  bool first = true;

  while (pos < clean.size()) {
    QString frag;
    int payloadSize;

    if (first) {
      // Premier fragment : header "XXYY " + payload (8 chars)
      QString header = senderId + targetId + " ";
      payloadSize = FIRST_PAYLOAD;
      QString payload = clean.mid(pos, payloadSize);
      frag = header + payload;
      first = false;
    } else {
      // Fragments suivants : payload complet (13 chars)
      payloadSize = SLOT_SIZE;
      frag = clean.mid(pos, payloadSize);
    }

    result.append(frag);
    pos += payloadSize;
  }

  return result;
}

// --- Fragmentation broadcast (avec /AR sur le dernier fragment) ---

QStringList ChatProtocol::fragmentBroadcast(const QString &senderId,
                                            const QString &targetId,
                                            const QString &text)
{
  QStringList result;
  // Réserver 3 chars pour /AR dans le dernier fragment
  QString clean = filterFT8Text(text, MAX_MESSAGE_LEN);
  if (clean.isEmpty()) return result;

  int pos = 0;
  bool first = true;

  while (pos < clean.size()) {
    QString frag;
    int payloadSize;

    if (first) {
      QString header = senderId + targetId + " ";
      payloadSize = FIRST_PAYLOAD;
      QString payload = clean.mid(pos, payloadSize);
      frag = header + payload;
      first = false;
    } else {
      payloadSize = SLOT_SIZE;
      frag = clean.mid(pos, payloadSize);
    }

    result.append(frag);
    pos += payloadSize;
  }

  // Ajouter /AR au dernier fragment
  if (!result.isEmpty()) {
    QString &last = result.last();
    // Si le dernier fragment + /AR dépasse 13 chars, on crée un fragment supplémentaire
    if (last.size() + 3 <= SLOT_SIZE) {
      // Padder avec des espaces puis ajouter /AR
      last = last.leftJustified(SLOT_SIZE - 3, ' ') + "/AR";
    } else {
      // Le dernier fragment est plein, ajouter un fragment "/AR" séparé
      result.append(QString("          /AR"));  // 10 espaces + /AR = 13
    }
  }

  return result;
}

// --- Constructeur ---

ChatProtocol::ChatProtocol(QObject *parent)
  : QObject(parent)
  , m_state(Idle)
  , m_fragIndex(0)
  , m_retryCount(0)
  , m_broadcastMode(false)
  , m_directTxCurrentFrag(-1)
{
  m_timeoutTimer.setSingleShot(true);
  m_timeoutTimer.setInterval(TIMEOUT_MS);
  connect(&m_timeoutTimer, &QTimer::timeout, this, &ChatProtocol::onTimeout);

  // Timer RX : si pas de nouveau fragment pendant 45s, message considéré complet
  m_rxCompleteTimer.setSingleShot(true);
  m_rxCompleteTimer.setInterval(45000);
  connect(&m_rxCompleteTimer, &QTimer::timeout, this, &ChatProtocol::onRxComplete);

  // Timer de suivi émission directe (tick toutes les 500ms)
  m_directTxTracker.setInterval(500);
  connect(&m_directTxTracker, &QTimer::timeout, this, &ChatProtocol::onDirectTxTick);
}

void ChatProtocol::setMyId(const QString &id)
{
  m_myId = id.leftJustified(2, '0').left(2);
}

void ChatProtocol::setState(State s)
{
  if (m_state != s) {
    m_state = s;
    emit stateChanged(s);
  }
}

void ChatProtocol::reset()
{
  m_fragments.clear();
  m_fragIndex = 0;
  m_lastSent.clear();
  m_retryCount = 0;
  m_broadcastMode = false;
  m_targetId.clear();
  m_rxSenderId.clear();
  m_echoText.clear();
  m_rxPayloads.clear();
  m_timeoutTimer.stop();
  m_rxCompleteTimer.stop();
  m_directTxTracker.stop();
  m_directTxCurrentFrag = -1;
  setState(Idle);
}

// ========== EXPEDITEUR ==========

void ChatProtocol::sendMessage(const QString &targetId, const QString &text)
{
  // Annuler toute session en cours
  reset();

  m_targetId = targetId.leftJustified(2, '0').left(2);
  m_fragments = fragmentMessage(m_myId, m_targetId, text);
  m_fragIndex = 0;
  m_retryCount = 0;

  if (m_fragments.isEmpty()) return;

  setState(SendingFragment);
  m_timeoutTimer.start();

  emit statusMessage(tr("Envoi vers %1 (%2 fragment(s))")
                      .arg(m_targetId)
                      .arg(m_fragments.size()));
}

void ChatProtocol::sendBroadcast(const QString &targetId, const QString &text)
{
  reset();

  m_broadcastMode = true;
  m_targetId = targetId.leftJustified(2, '0').left(2);
  m_fragments = fragmentBroadcast(m_myId, m_targetId, text);
  m_fragIndex = 0;

  if (m_fragments.isEmpty()) return;

  setState(Broadcasting);
  m_timeoutTimer.start();

  emit statusMessage(tr("Broadcast vers %1 (%2 fragment(s))")
                      .arg(m_targetId)
                      .arg(m_fragments.size()));
}

bool ChatProtocol::hasDataToSend() const
{
  return m_state == SendingFragment || m_state == EchoReady || m_state == Broadcasting;
}

QString ChatProtocol::nextTxText()
{
  if (m_state == SendingFragment) {
    // Expéditeur (mode écho) : envoyer le fragment courant
    if (m_fragIndex >= m_fragments.size()) {
      setState(Idle);
      return QString();
    }
    m_lastSent = m_fragments.at(m_fragIndex);
    setState(WaitingEcho);

    emit fragmentProgress(m_fragIndex + 1, m_fragments.size(), false);
    emit statusMessage(tr("TX fragment %1/%2")
                        .arg(m_fragIndex + 1)
                        .arg(m_fragments.size()));
    return m_lastSent;
  }

  if (m_state == Broadcasting) {
    // Expéditeur (mode broadcast) : envoyer le fragment courant puis passer au suivant
    if (m_fragIndex >= m_fragments.size()) {
      setState(Idle);
      return QString();
    }
    QString frag = m_fragments.at(m_fragIndex);
    m_fragIndex++;

    emit fragmentProgress(m_fragIndex, m_fragments.size(), false);
    emit statusMessage(tr("CQ %1/%2").arg(m_fragIndex).arg(m_fragments.size()));

    if (m_fragIndex >= m_fragments.size()) {
      // Dernier fragment envoyé
      m_timeoutTimer.stop();
      setState(Complete);
      emit messageSentOk(m_targetId);
      emit statusMessage(tr("Broadcast termine vers %1").arg(m_targetId));
      QTimer::singleShot(2000, this, [this]() {
        if (m_state == Complete) setState(Idle);
      });
    }

    return frag;
  }

  if (m_state == EchoReady) {
    // Récepteur : renvoyer l'écho
    QString echo = m_echoText;
    setState(WaitingNext);
    m_rxCompleteTimer.start();

    emit fragmentProgress(m_rxPayloads.size(), 0, true);
    emit statusMessage(tr("Echo envoyé"));
    return echo;
  }

  return QString();
}

// ========== RECEPTION / TRAITEMENT ==========

void ChatProtocol::processIncoming(const QString &freeText)
{
  QString text = freeText.trimmed();
  if (text.isEmpty()) return;

  // --- CAS 1 : On est expéditeur, on attend un écho ---
  if (m_state == WaitingEcho) {
    // Comparer le texte reçu avec le dernier fragment envoyé
    QString expected = m_lastSent.trimmed();
    QString received = text.trimmed();

    // Tronquer au même nombre de caractères pour la comparaison
    // (le décodeur peut ajouter/retirer des espaces)
    int len = qMin(expected.size(), received.size());
    bool match = (expected.left(len) == received.left(len));

    if (match) {
      // Echo confirmé !
      emit statusMessage(tr("Echo OK pour fragment %1/%2")
                          .arg(m_fragIndex + 1)
                          .arg(m_fragments.size()));
      m_retryCount = 0;
      m_fragIndex++;

      if (m_fragIndex >= m_fragments.size()) {
        // Tous les fragments confirmés !
        m_timeoutTimer.stop();
        setState(Complete);
        emit messageSentOk(m_targetId);
        emit statusMessage(tr("Message envoyé avec succès à %1").arg(m_targetId));
        // Retour à Idle après un court délai
        QTimer::singleShot(2000, this, [this]() {
          if (m_state == Complete) setState(Idle);
        });
      } else {
        // Prochain fragment
        setState(SendingFragment);
      }
    } else {
      // Echo ne correspond pas → retransmission
      m_retryCount++;
      if (m_retryCount >= MAX_RETRIES) {
        emit statusMessage(tr("Trop de retransmissions, abandon"));
        reset();
        return;
      }
      emit statusMessage(tr("Echo incorrect, retransmission (%1/%2)")
                          .arg(m_retryCount)
                          .arg(MAX_RETRIES));
      setState(SendingFragment);  // Retransmettre le même fragment
    }
    return;
  }

  // --- CAS 2 : Message avec header adressé à nous ---
  if (isHeader(text)) {
    QString target = headerTarget(text);
    QString sender = headerSender(text);

    if (target == m_myId) {
      // C'est pour nous !
      m_rxSenderId = sender;

      // Extraire le payload et commencer/recommencer le réassemblage
      QString payload = headerPayload(text);
      m_rxPayloads.clear();

      // Vérifier si c'est un broadcast (premier fragment avec /AR = message court)
      if (endsWithAR(payload)) {
        // Message broadcast complet en 1 fragment
        m_rxPayloads.append(stripAR(payload));
        deliverReceivedMessage();
        return;
      }

      m_rxPayloads.append(payload);
      m_echoText = text;

      setState(EchoReady);
      emit statusMessage(tr("Reçu de %1, écho en préparation").arg(sender));
      return;
    }
    // Pas pour nous, ignorer
    return;
  }

  // --- CAS 3 : On est récepteur en attente du prochain fragment ---
  if ((m_state == WaitingNext || m_state == EchoReady) && !m_rxSenderId.isEmpty()) {
    // Fragment de continuation (pas de header)
    m_rxCompleteTimer.stop();

    // Vérifier si c'est le dernier fragment broadcast (/AR)
    if (endsWithAR(text)) {
      m_rxPayloads.append(stripAR(text));
      deliverReceivedMessage();
      return;
    }

    m_echoText = text;
    m_rxPayloads.append(text);

    setState(EchoReady);
    emit statusMessage(tr("Fragment suite de %1, écho en préparation").arg(m_rxSenderId));
    return;
  }

  // --- CAS 4 : Message non reconnu, ignorer ---
}

// ========== CONTRÔLE ==========

void ChatProtocol::haltTx()
{
  reset();
  emit statusMessage(tr("Transmission arrêtée"));
}

void ChatProtocol::notifyDirectTxComplete()
{
  m_directTxTracker.stop();
  // Emettre le dernier fragment comme "envoyé"
  if (!m_fragments.isEmpty()) {
    int total = m_fragments.size();
    emit directFragmentStarted(total, total, m_fragments.last(), QString());
    emit fragmentProgress(total, total, false);
  }
  setState(Complete);
  emit messageSentOk(m_targetId);
  emit statusMessage(tr("Emission directe terminée vers %1").arg(m_targetId));
  emit directTxComplete();
  QTimer::singleShot(2000, this, [this]() {
    if (m_state == Complete) setState(Idle);
  });
}

// ========== TIMEOUTS ==========

void ChatProtocol::onTimeout()
{
  if (m_state != Idle && m_state != Complete) {
    if (m_state == Broadcasting) {
      emit statusMessage(tr("Timeout, abandon du broadcast"));
    } else {
      emit statusMessage(tr("Timeout, abandon de la transmission"));
    }
    reset();
  }
}

void ChatProtocol::deliverReceivedMessage()
{
  if (m_rxPayloads.isEmpty()) return;

  // Réassembler le message (FT8 supprime les espaces de fin de chaque fragment)
  QString fullMessage;
  for (const QString &payload : m_rxPayloads) {
    if (!fullMessage.isEmpty() && !fullMessage.endsWith(' ') && !payload.startsWith(' ')) {
      fullMessage.append(' ');
    }
    fullMessage.append(payload);
  }
  fullMessage = fullMessage.trimmed();

  QString sender = m_rxSenderId;

  // Reset état récepteur
  m_rxSenderId.clear();
  m_rxPayloads.clear();
  m_echoText.clear();
  m_rxCompleteTimer.stop();
  setState(Idle);

  emit messageReceived(sender, fullMessage);
  emit statusMessage(tr("Message complet reçu de %1").arg(sender));
}

void ChatProtocol::onRxComplete()
{
  // Côté récepteur : pas de nouveau fragment reçu → message complet
  if ((m_state == WaitingNext || m_state == Idle) && !m_rxPayloads.isEmpty()) {
    deliverReceivedMessage();
  }
}

// ========== EMISSION DIRECTE (N trames FT8 concaténées) ==========

void ChatProtocol::sendDirect(const QString &targetId, const QString &text, double txFreq)
{
  reset();

  m_broadcastMode = true;
  m_targetId = targetId.leftJustified(2, '0').left(2);
  m_fragments = fragmentBroadcast(m_myId, m_targetId, text);

  if (m_fragments.isEmpty()) return;

  int totalSymbols = prepareTxWaveform(m_fragments, txFreq);
  if (totalSymbols <= 0) {
    emit statusMessage(tr("Erreur encodage FT8"));
    reset();
    return;
  }

  setState(DirectTx);

  emit statusMessage(tr("Emission directe vers %1 (%2 fragment(s), %3s)")
                      .arg(m_targetId)
                      .arg(m_fragments.size())
                      .arg(m_fragments.size() * 15));
  emit fragmentProgress(0, m_fragments.size(), false);
  emit directTxReady(totalSymbols, m_fragments.size());
}

int ChatProtocol::prepareTxWaveform(const QStringList &fragments, double txFreq)
{
  int offset = 0;  // sample offset in foxcom_.wave[]

  for (int i = 0; i < fragments.size(); ++i) {
    // Prepare the 37-char padded message buffer for Fortran
    QByteArray msgBa = fragments.at(i).toLatin1().leftJustified(37, ' ');
    char message[37];
    std::memcpy(message, msgBa.constData(), 37);

    char msgsent[37];
    std::memset(msgsent, ' ', 37);
    char ft8msgbits[77];
    std::memset(ft8msgbits, 0, 77);
    int itone[79];
    std::memset(itone, 0, sizeof(itone));

    int i3 = 0, n3 = 0;

    // Encode the message into tone sequence
    genft8_(message, &i3, &n3, msgsent, ft8msgbits, itone,
            (fortran_charlen_t)37, (fortran_charlen_t)37);

    // Generate waveform from tones
    int nsym = FT8_NSYM;        // 79 symbols
    int nsps = FT8_NSPS;        // 7680 samples/symbol
    float bt = 2.0f;
    float fsample = 48000.0f;
    float f0 = static_cast<float>(txFreq);
    int icmplx = 0;
    int nwave = nsym * nsps;     // 606720 samples

    // Write directly into foxcom_.wave[] at the correct offset
    gen_ft8wave_(itone, &nsym, &nsps, &bt, &fsample, &f0,
                 &foxcom_.wave[offset], &foxcom_.wave[offset],
                 &icmplx, &nwave);

    // Fill the remaining gap with silence (2.36s) to complete the 15s period
    for (int j = nwave; j < SAMPLES_PER_PERIOD; ++j) {
      foxcom_.wave[offset + j] = 0.0f;
    }

    offset += SAMPLES_PER_PERIOD;

    qDebug() << "ChatProtocol: encoded fragment" << (i + 1) << "/" << fragments.size()
             << ":" << fragments.at(i) << "offset=" << offset;
  }

  // Calculate total symbols for the Modulator
  // totalSamples = N * SAMPLES_PER_PERIOD
  // symbolsLength such that symbolsLength * 4 * nsps_at_12kHz covers totalSamples
  // At 48kHz: totalSamples = symbolsLength * 4.0 * 1920 (framesPerSymbol at 48kHz from nsps_at_12kHz=1920)
  // Wait - the Modulator uses m_nsps = framesPerSymbol * 4 (upsampled) = 1920*4 = 7680
  // i1 = 4 * symbolsLength * m_nsps - 1  (in readData, at 48kHz)
  // So: totalSamples_needed = 4 * symbolsLength * 1920 (at 48kHz: 4*symbolsLength*1920)
  // Actually at 48kHz the Modulator computes: i1 = qRound(4.0 * symbolsLength * m_nsps) - 1
  // where m_nsps = framesPerSymbol * (frameRate/sampleRate) for FT8 = 1920 * (48000/12000) = 7680
  // Wait no, m_nsps is set from framesPerSymbol parameter directly: m_nsps = framesPerSymbol
  // i1 = 4 * symbolsLength * framesPerSymbol - 1
  // We need: 4 * symbolsLength * 1920 >= totalSamples at 48kHz
  // totalSamples = fragments.size() * SAMPLES_PER_PERIOD = fragments.size() * 720000
  // symbolsLength = ceil(totalSamples / (4.0 * 1920))
  int totalSamples = fragments.size() * SAMPLES_PER_PERIOD;
  int totalSymbols = (totalSamples + 4 * 1920 - 1) / (4 * 1920);  // ceiling division

  qDebug() << "ChatProtocol: prepareTxWaveform done,"
           << fragments.size() << "fragments,"
           << totalSamples << "samples,"
           << totalSymbols << "symbols,"
           << (totalSamples / 48000.0) << "seconds";

  return totalSymbols;
}

void ChatProtocol::startDirectTxTracking()
{
  m_directTxCurrentFrag = -1;  // Force le premier tick à émettre le signal
  m_directTxElapsed.start();
  m_directTxTracker.start();
  // Tick immédiat pour afficher le fragment 1
  onDirectTxTick();
}

void ChatProtocol::onDirectTxTick()
{
  if (m_state != DirectTx || m_fragments.isEmpty()) {
    m_directTxTracker.stop();
    return;
  }

  // Calculer quel fragment est en cours basé sur le temps écoulé
  qint64 elapsedMs = m_directTxElapsed.elapsed();
  int fragIndex = static_cast<int>(elapsedMs / 15000);  // 15s par fragment

  if (fragIndex >= m_fragments.size()) {
    fragIndex = m_fragments.size() - 1;
  }

  // Si on a changé de fragment, émettre le signal
  if (fragIndex != m_directTxCurrentFrag) {
    m_directTxCurrentFrag = fragIndex;

    int current = fragIndex + 1;
    int total = m_fragments.size();
    QString currentText = m_fragments.at(fragIndex);
    QString nextText;
    if (fragIndex + 1 < m_fragments.size()) {
      nextText = m_fragments.at(fragIndex + 1);
    }

    emit directFragmentStarted(current, total, currentText, nextText);
    emit fragmentProgress(current, total, false);

    int secsRemaining = (total - current) * 15 + 15 - static_cast<int>((elapsedMs % 15000) / 1000);
    emit statusMessage(tr("TX direct %1/%2 — reste %3s")
                        .arg(current).arg(total).arg(secsRemaining));
  }
}
