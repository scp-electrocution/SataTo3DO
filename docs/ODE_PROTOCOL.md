# Protocollo ODE LOCODE — interfaccia menu ⇄ firmware

Questo documento descrive il protocollo che il firmware SataTo3DO espone al
programma di menu (`boot.iso`) che gira **sulla 3DO**. È ricavato dal codice
sorgente (`src/interfaces/3DO.c`, `MSC.c`) e serve a scrivere un menu
alternativo che resti compatibile con il firmware attuale.

> Nota: il protocollo "base" (READ_ID, READ_TOC, READ_DATA, ecc.) è quello del
> lettore CD originale della 3DO ed è usato dalla BIOS/SO per avviare e leggere
> un'immagine. I comandi **estesi** (0x93, 0xC0–0xF0) sono un'aggiunta di questo
> ODE e sono ciò che un menu usa per navigare e lanciare i giochi.

## 1. Modello generale

- Ogni comando dal lato 3DO è formato da **1 byte di opcode + 6 byte di
  parametri** (7 byte in totale). Il firmware consuma sempre i 6 parametri,
  anche quando non risponde.
- La maggior parte dei comandi risponde con un pacchetto che inizia con
  l'opcode e termina con il byte di **status**. Alcuni (GET_TOC_LIST,
  READ_BUFFER) rispondono con un blocco dati "grezzo" da 2048 byte.
- Il byte di **status** usa i bit:
  `TRAY_IN=0x80, DISC_PRESENT=0x40, SPINNING=0x20, CHECK_ERROR=0x10,
  DOUBLE_SPEED=0x02, DISC_RDY=0x01`.
- Tutti i campi multibyte nelle strutture TOC sono **big-endian**.

## 2. Identificazione dell'ODE

### `0x93` EXT_ID
Permette al menu di riconoscere che gira su questo ODE.

Risposta: `[0x93, location, 'L','O','C','O','D','E', revMaj, revMin, revPatch, status]`

- `location`: dove si trova il boot.iso — `1`=microSD, `2`=USB MSC, `4`=flash,
  `8`=sata. Il firmware attuale risponde `1`.
- revisione attuale: `1.1.0`.

Un menu dovrebbe inviare EXT_ID all'avvio: se riceve "LOCODE" sa che i comandi
estesi sono disponibili.

## 3. Navigazione: il buffer TOC

Il menu vede i dispositivi e i loro contenuti come un albero di cartelle. Ogni
livello è una lista di **entry** serializzate in un buffer da 2048 byte.

### Formato di una entry
```
offset  size  campo
  0      4    flags        (big-endian)
  4      4    toc_id       (big-endian)
  8      4    name_length  (big-endian, include il terminatore '\0')
 12      N    name         (name_length byte, terminato da '\0')
```
Le entry sono concatenate. La **fine della lista** è segnalata da un `flags`
pari a `0xFFFFFFFF` (`TOC_FLAG_INVALID`).

- `flags`: `0x1`=file (gioco), `0x2`=cartella/dispositivo.
- `toc_id`: identificatore dell'entry. **Byte alto** (bit 24-31) = indice del
  dispositivo; **24 bit bassi** = indice dell'entry nella cartella corrente
  (1-based). Un `toc_id` con 24 bit bassi a `0` rappresenta il **dispositivo
  stesso**. Il valore `0xFFFFFF` nei 24 bit bassi è l'entry speciale ".."
  (risali di un livello).

### `0xD1` GET_TOC_LIST — costruisci ed esporta la lista
Parametri: `data_in[0..3]` = `index` (toc_id del livello da elencare),
`data_in[4..5]` = `nb` (numero massimo di entry; `0` = nessun limite pratico).

Effetto: il firmware ricostruisce il buffer TOC da 2048 byte per quel livello e
lo invia come **blocco grezzo da 2048 byte** (preceduto dall'opcode nel
pacchetto di status). Il menu lo legge e lo scorre con il formato qui sopra.

> È il comando principale: il menu lo chiama per avere l'elenco corrente.

### `0xC1` GET_TOC — rileggi 16 byte del buffer
Parametri: `index` (4 byte) + `offset` (2 byte, offset **in byte** dentro il
buffer TOC). Risposta: `[0xC1, 16 byte del buffer a partire da offset, status]`.

Utile se il menu preferisce leggere il buffer a blocchi invece che tutto in una
volta. Se `offset==0` il firmware rigenera prima il buffer.

### `0xC0` CHANGE_TOC — entra/esci da una cartella
Parametri: `index` (4 byte) = `toc_id` dell'entry.

- `toc_id` di una cartella → ci entra.
- `toc_id` con 24 bit bassi a `0` e un indice di dispositivo → apre la radice di
  quel dispositivo.
- `toc_id` con 24 bit bassi `0xFFFFFF` → risale di un livello ("..").

Dopo un CHANGE_TOC il menu richiama GET_TOC_LIST per avere il nuovo contenuto.

## 4. Lancio dei giochi: la playlist

Il menu compone una coda di immagini da avviare (una o più), poi la lancia.

### `0xC3` CLEAR_PLAYLIST
Svuota la playlist. Risposta `[0xC3, status]`.

### `0xC4` ADD_PLAYLIST
Parametri: `entry` (4 byte) = `toc_id` di un file-gioco (o di un dispositivo CD).
Risposta: `[0xC4, valid, added, status]`.
- `valid`=1 se l'entry è un gioco lanciabile; `added`=1 se è stata aggiunta
  (la playlist ha un massimo di 16 voci).

### `0xC5` LAUNCH_PLAYLIST
Avvia la playlist: il firmware segnala un "cambio disco" alla 3DO, che rilancia
caricando la prima immagine della coda. Risposta `[0xC5, status]`, poi segue il
media change.

Flusso tipico del menu per avviare un gioco:
`CLEAR_PLAYLIST → ADD_PLAYLIST(toc_id) → LAUNCH_PLAYLIST`.

## 5. File di appoggio (opzionali, es. salvataggi del menu)

Permettono al menu di leggere/scrivere piccoli file nella cartella corrente.
Buffer di scambio: `FILE_BUFFER`, 2048 byte lato firmware.

- `0xE1` OPEN_FILE — `data_in[0]`=0 per aprire per nome (nome già caricato in
  FILE_BUFFER), `data_in[1..2]`=lunghezza nome, `data_in[5]`≠0 = apertura in
  scrittura. Risposta `[0xE1, success, status]`.
- `0xE3` READ_FILE_BYTE — `data_in[0..1]`=lunghezza. Legge in FILE_BUFFER.
  Risposta `[0xE3, nread_hi, nread_lo, status]` (byte realmente letti).
- `0xE4` WRITE_FILE_BYTE — come sopra, scrive da FILE_BUFFER. Risposta con i
  byte realmente scritti.
- `0xE5` CLOSE_FILE — `[0xE5, status]`.
- `0xE6` WRITE_BUFFER_OFFSET — carica 4 byte (`data_in[2..5]`) in FILE_BUFFER a
  `offset = ((data_in[0]<<8)|data_in[1])<<2`. Serve per riempire FILE_BUFFER
  (es. il nome file) prima di OPEN/WRITE.
- `0xE7` READ_BUFFER — invia i 2048 byte di FILE_BUFFER come blocco grezzo.
- `0xE0` CREATE_FILE, `0xE2` SEEK_FILE: **non supportati** (CREATE risponde
  sempre fallimento, SEEK è un no-op).

## 6. Comandi riservati / non implementati
- `0xC2` GET_DESC — risponde solo con lo status (nessun payload descrittivo).
- `0xF0` UPDATE_ODE — placeholder per l'aggiornamento firmware da menu.

## 7. Note di compatibilità per un menu "da zero"
1. Invia EXT_ID all'avvio e procedi con i comandi estesi solo se ricevi
   "LOCODE".
2. Al primo ingresso chiama GET_TOC_LIST con `index=0` per avere la radice:
   con un solo dispositivo mostra direttamente i file; con più dispositivi la
   radice è la lista dei dispositivi (`CD 0`, `USB 0`, ...).
3. Per ogni entry decidi in base a `flags`: cartella/dispositivo → CHANGE_TOC;
   file → candidato per ADD_PLAYLIST.
4. Gestisci sempre l'entry ".." (`toc_id` con 24 bit bassi = `0xFFFFFF`) per
   risalire.
5. I nomi hanno un limite di 128 caratteri (lato firmware, `TOC_NAME_LIMIT`).
6. Dopo LAUNCH_PLAYLIST aspettati il reset/cambio disco: il tuo menu verrà
   scaricato e partirà il gioco.

Opcode (riassunto): EXT_ID=0x93, CHANGE_TOC=0xC0, GET_TOC=0xC1, GET_DESC=0xC2,
CLEAR_PLAYLIST=0xC3, ADD_PLAYLIST=0xC4, LAUNCH_PLAYLIST=0xC5, GET_TOC_LIST=0xD1,
CREATE_FILE=0xE0, OPEN_FILE=0xE1, SEEK_FILE=0xE2, READ_FILE_BYTE=0xE3,
WRITE_FILE_BYTE=0xE4, CLOSE_FILE=0xE5, WRITE_BUFFER_OFFSET=0xE6,
READ_BUFFER=0xE7, UPDATE_ODE=0xF0.
