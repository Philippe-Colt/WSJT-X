# HF Chat - WSJT-X modifie

## Projet

Client de chat HF integre dans WSJT-X 2.7.0-rc4. Utilise les messages free-text
FT8 (13 chars max) pour transmettre des messages fragmentes entre stations.
Identifiants stations sur 2 chiffres (01-99), pas d'indicatifs radioamateur.

## Build

```bash
cmake --build /tmp/wsjtx-mod/build --target wsjtx -j$(nproc)
```

Pour recompiler un seul fichier force :
```bash
rm -f /tmp/wsjtx-mod/build/CMakeFiles/wsjtx.dir/widgets/mainwindow.cpp.o
cmake --build /tmp/wsjtx-mod/build --target wsjtx -j$(nproc)
```

IMPORTANT : CMake ne detecte pas toujours les changements. Si le binaire
n'est pas recompile, supprimer le .o manuellement ou faire `touch` sur le .cpp.

Executable : `/tmp/wsjtx-mod/build/wsjtx`

## Architecture des fichiers Chat

| Fichier | Role |
|---------|------|
| `ChatProtocol.h/cpp` | Protocole : fragmentation, echo, broadcast, direct TX, reassemblage RX |
| `widgets/ChatWidget.h/cpp` | UI du chat : historique, input, barre de progression, status |
| `widgets/mainwindow.cpp` | Integration : interception decodes FT8, controle TX, connexion signaux |
| `widgets/mainwindow.h` | Membres chat : `m_chatProtocol`, `m_chatWidget`, `m_chatDock`, `m_chatTxActive` |
| `commons.h` | Structure `foxcom_` : buffer `wave[]` partage C++/Fortran pour TX direct |
| `Decoder/decodedtext.h/cpp` | Parsing des lignes decodees par jt9 |

## Protocole Chat

### Format des fragments (13 chars FT8 free-text)

- **Premier fragment** : `XXYY PAYLOAD` (header 5 chars + 8 chars payload)
  - XX = sender ID, YY = target ID
  - Ex: `0102 HELLO WO`
- **Fragments suivants** : 13 chars payload pur, ex: `RLD CMT CA V`
- **Fin broadcast** : dernier fragment se termine par `/AR`

### Modes d'envoi

1. **Echo (point a point)** : TX fragment → attente echo → echo OK → fragment suivant. Max 5 retries.
2. **Broadcast** : TX continu sans echo, `/AR` en fin.
3. **Direct TX** : N trames FT8 pre-encodees dans `foxcom_.wave[]`, emission continue.

### Etats (`ChatProtocol::State`)

`Idle` → `SendingFragment` → `WaitingEcho` → ... → `Complete` → `Idle`
`Idle` → `Broadcasting` → `Complete` → `Idle`
`Idle` → `DirectTx` → `Complete` → `Idle`
Recepteur : `Idle` → `EchoReady` → `WaitingNext` → `EchoReady` → ... → `Idle`

### Constantes

- `SLOT_SIZE = 13`, `HEADER_SIZE = 5`, `FIRST_PAYLOAD = 8`
- `MAX_MESSAGE_LEN = 99`, `MAX_RETRIES = 5`, `TIMEOUT_MS = 90000`
- FT8 : 79 symboles, 7680 samples/symbole @ 48kHz, periode 15s (720000 samples)

## Flux de donnees

### Envoi (TX)
```
ChatWidget::onSendClicked()
  → emit directSendRequested(targetId, text)
  → MainWindow::onChatDirectSendRequested()
    → ChatProtocol::sendDirect(targetId, text, txFreq)
      → prepareTxWaveform() : genft8_() + gen_ft8wave_() → foxcom_.wave[]
      → emit directTxReady(totalSymbols, numFragments)
    → MainWindow::onChatDirectTx()
      → Attend debut cycle 15s
      → g_iptt=1, emit sendMessage(..., toneSpacing=-3)  // -3 = lire foxcom_.wave[]
      → startDirectTxTracking()
      → Schedule onChatDirectTxDone()
```

### Reception (RX)
```
ft8_decode (Fortran) → jt9 stdout → MainWindow::readFromStdout()
  → Extraction du message : raw.mid(22 + pad).trimmed()
    (pad = 2 si secondes presentes dans le timestamp, sinon 0)
  → Si chat visible, mode FT8/FT4, pas un TX :
    → ChatProtocol::processIncoming(msg)
      → isHeader() → headerTarget() == m_myId ?
        → Oui : accumuler payload, setState(EchoReady)
        → Fragment suivant : CAS 3 (state EchoReady ou WaitingNext)
        → /AR detecte → deliverReceivedMessage()
          → emit messageReceived(sender, fullText)
      → ChatWidget::onMessageReceived() → appendChat()
```

### Interception dans mainwindow.cpp (~ligne 4212)
```cpp
// NE PAS utiliser isStandardMessage() : les messages free-text FT8
// passent stdmsg_() et sont consideres "standard" par WSJT-X.
// On laisse processIncoming filtrer par format header + target ID.
if (m_chatDock && m_chatDock->isVisible()
    && (m_mode == "FT8" || m_mode == "FT4")
    && !decodedtext.isTX()) {
  QString raw = decodedtext.string();
  int pad = raw.indexOf(" ") > 4 ? 2 : 0;
  QString msg = raw.mid(22 + pad).trimmed();
  if (!msg.isEmpty()) {
    m_chatProtocol->processIncoming(msg);
  }
}
```

### Pieges connus

- **isStandardMessage()** : retourne true pour les free-text FT8 (stdmsg_ roundtrip OK).
  NE PAS filtrer avec `!isStandardMessage()` sinon les messages chat sont bloques.
- **messageWords()** : pour les messages "standard", utilise une regex qui ne matche pas
  le format chat. Extraire le message directement depuis `decodedtext.string().mid(22+pad)`.
- **Assemblage fragments** : FT8 supprime les espaces de fin de chaque fragment.
  `deliverReceivedMessage()` ajoute un espace entre fragments si necessaire.
- **Couleurs chat** : fond blanc par defaut, ne pas utiliser `Qt::white` pour le texte.
  RX = bleu `QColor(0, 0, 200)`, TX echo = rouge `QColor(200, 0, 0)`,
  TX broadcast = orange `QColor(255, 140, 0)`.
- **qDebug()** : WSJT-X installe un message handler custom qui supprime qDebug().
  Utiliser `fprintf(stderr, ...)` + `fflush(stderr)` pour le debug.

## HF Chat Mode (mainwindow.cpp, apres readSettings ~ligne 950)

Au demarrage, le mode HF Chat :
- Cache les controles QSO inutiles (logQSO, DX, tabWidget, modes, etc.)
- Force le mode FT8 (`on_actionFT8_triggered()`)
- Place le dock chat a droite (`Qt::RightDockWidgetArea`) uniquement
- Resize le dock a 50% de la largeur via `QTimer::singleShot(200ms, resizeDocks)`
  (doit etre differe car `width()` vaut 0 dans le constructeur)
- Compacte la grille (`gridLayout_5` colonnes 4-6 stretch=0)
- Surcharge le `restoreState()` sauvegarde en re-appelant `addDockWidget`

## UI du ChatWidget

- `QTextEdit m_chatHistory` : historique (Courier 9, read-only)
- `QLineEdit m_myIdField` / `m_targetIdField` : IDs station (01-99)
- `QLineEdit m_inputField` : saisie message (max 99 chars)
- `QPushButton` : TX (echo), CQ (broadcast), Halt (arret)
- `QProgressBar m_txProgress` + `QLabel m_fragmentLabel` : progression TX direct
- `QLabel m_statusLabel` : etat protocole

### Affichage messages
- **TX** : message affiche UNE SEULE FOIS quand transmission complete (`onMessageSentOk`)
  - Texte sauvegarde dans `m_pendingSentText` / `m_pendingTarget` / `m_pendingIsBroadcast` au clic
  - Pas de lignes intermediaires (fragments, echos) dans le chat
  - Barre de progression et label fragment visibles pendant TX
- **RX** : message affiche une seule ligne quand reassemblage complet (`onMessageReceived`)
- Couleurs : rouge (TX echo), orange (TX broadcast), bleu (RX)

## Test de reception simule

Generer des fragments FT8 audio avec `ft8sim` et les jouer via loopback PulseAudio :
```bash
# Creer un sink virtuel
pactl load-module module-null-sink sink_name=ft8_loopback sink_properties=device.description="FT8_Loopback"
# Dans WSJT-X Settings > Audio : selectionner "FT8_Loopback Monitor" comme input

# Generer les fragments
cd /tmp/ft8test
/tmp/wsjtx-mod/build/ft8sim "0201 BONJOUR " 1000.0 0.0 0.0 1.0 1 -5
# Jouer synchronise sur un cycle FT8 (0/15/30/45s)
paplay --device=ft8_loopback fichier.wav
```

## Fortran (encodage FT8)

```cpp
extern "C" {
  void genft8_(char* msg, int* i3, int* n3, char* msgsent,
               char ft8msgbits[], int itone[],
               fortran_charlen_t, fortran_charlen_t);
  void gen_ft8wave_(int itone[], int* nsym, int* nsps, float* bt,
                    float* fsample, float* f0, float xjunk[],
                    float wave[], int* icmplx, int* nwave);
}
```

## foxcom_ (commons.h)

```c
extern struct {
  float wave[(160+2)*134400*4]; // ~87M floats, ~1812s @ 48kHz
  int   nslots;
  int   nfreq;
  int   i3bit[5];
  char  cmsg[5][40];
  char  mycall[12];
} foxcom_;
```

Le chat ecrit les trames FT8 pre-encodees dans `foxcom_.wave[]` a des offsets
de `SAMPLES_PER_PERIOD` (720000) par fragment. Le Modulator lit ce buffer quand
`toneSpacing < 0` (valeur -3).

## Dependances build

Qt5 (Widgets, SerialPort, Multimedia, PrintSupport, Sql), GFortran,
FFTW3 (single), Hamlib, Boost (log_setup, log), CMake 3.7.2+
