#include "remote_i18n.h"
#include <string.h>

remote_lang_t g_language = REMOTE_LANG_EN;

static const char * const g_remote_strings[STR_COUNT][REMOTE_LANG_COUNT] = {
    /* STR_ACTIVITIES */
    { "ACTIVITIES", "AKTIVITAETEN", "ACTIVITES", "ACTIVITEITEN", "ACTIVIDADES", "ATTIVITA" },
    /* STR_DEVICES */
    { "DEVICES", "GERAETE", "DISPOSITIFS", "APPARATEN", "DISPOSITIVOS", "DISPOSITIVI" },
    /* STR_COMMANDS */
    { "COMMANDS", "BEFEHLE", "COMMANDES", "OPDRACHTEN", "COMANDOS", "COMANDI" },
    /* STR_SETTINGS */
    { "SETTINGS", "EINSTELLUNGEN", "PARAMETRES", "INSTELLINGEN", "AJUSTES", "IMPOSTAZIONI" },
    /* STR_RF_PAIRING */
    { "RF PAIRING", "RF KOPPLUNG", "PAIRAGE RF", "RF KOPPELING", "VINCULAR RF", "ABBINA RF" },
    /* STR_NOT_PAIRED */
    { "NOT PAIRED", "NICHT GEKOPPELT", "NON PAIRE", "NIET GEKOPPELD", "NO VINCULADO", "NON ABBINATO" },
    /* STR_NO_HUB */
    { "NO HUB", "KEIN HUB", "PAS DE HUB", "GEEN HUB", "SIN HUB", "NO HUB" },
    /* STR_TAB_ACT */
    { "ACT", "AKT", "ACT", "ACT", "ACT", "ATT" },
    /* STR_TAB_CMD */
    { "CMD", "BEF", "CMD", "OPD", "CMD", "CMD" },
    /* STR_TAB_DEV */
    { "DEV", "GER", "DIS", "APP", "DIS", "DIS" },

    /* STR_NO_ACTIVITIES */
    { "No Activities", "Keine Aktivitaeten", "Aucune activite", "Geen activiteiten", "Sin actividades", "Nessuna attivita" },
    /* STR_SYNCING_WITH_HUB */
    { "Syncing with Hub...", "Synchronisiere...", "Sync avec le Hub...", "Synchroniseren...", "Sincronizando...", "Sincronizzazione..." },
    /* STR_CONFIRM_PRESS_OK */
    { "CONFIRM? PRESS OK", "BESTAETIGEN? OK", "CONFIRMER? APPUYEZ OK", "BEVESTIGEN? DRUK OK", "CONFIRMAR? PULSE OK", "CONFERMA? PREMI OK" },
    /* STR_ACTIVE */
    { "ACTIVE", "AKTIV", "ACTIF", "ACTIEF", "ACTIVO", "ATTIVO" },
    /* STR_PRESS_OK_TO_CONFIRM */
    { "Press OK or tap to confirm", "OK oder tippen zum Bestaetigen", "Appuyez OK pour confirmer", "Druk OK om te bevestigen", "Pulse OK para confirmar", "Premi OK per confermare" },

    /* STR_SWITCHING_ACTIVITY */
    { "SWITCHING ACTIVITY", "AKTIVITAET WECHSELN", "CHANGEMENT D'ACTIVITE", "ACTIVITEIT WISSELEN", "CAMBIANDO ACTIVIDAD", "CAMBIO ATTIVITA" },
    /* STR_SWITCHING_TO */
    { "SWITCHING TO", "WECHSELN ZU", "PASSAGE A", "WISSELEN NAAR", "CAMBIANDO A", "PASSAGGIO A" },
    /* STR_CONFIGURING_DEVICES */
    { "Configuring devices", "Geraete konfigurieren", "Configuration appareils", "Apparaten instellen", "Configurando equipos", "Configuro dispositivi" },
    /* STR_POWERING_OFF_ALL */
    { "Powering off all devices", "Alle Geraete ausschalten", "Extinction appareils", "Alle apparaten uitschakelen", "Apagando todos los equipos", "Spegnimento dispositivi" },
    /* STR_EXECUTING_ACTIONS */
    { "Executing Actions", "Fuehre Aktionen aus", "Execution actions", "Acties uitvoeren", "Ejecutando acciones", "Esecuzione azioni" },
    /* STR_PLEASE_WAIT */
    { "Please wait...", "Bitte warten...", "Veuillez patienter...", "Even geduld...", "Por favor espere...", "Attendere..." },
    /* STR_SCREEN_LOCKED_SWITCH */
    { "Screen locked during switch", "Bildschirm gesperrt", "Ecran verrouille", "Scherm vergrendeld", "Pantalla bloqueada", "Schermo bloccato" },

    /* STR_NO_DEVICES */
    { "No Devices", "Keine Geraete", "Aucun appareil", "Geen apparaten", "Sin dispositivos", "Nessun dispositivo" },
    /* STR_NO_COMMANDS */
    { "No Commands", "Keine Befehle", "Aucune commande", "Geen opdrachten", "Sin comandos", "Nessun comando" },
    /* STR_CONFIGURE_IN_WEBUI */
    { "Configure in WebUI", "In WebUI einrichten", "Configurer dans WebUI", "Instellen in WebUI", "Configurar en WebUI", "Configura in WebUI" },
    /* STR_FOR_THIS_ACTIVITY */
    { "for this activity", "fuer diese Aktivitaet", "pour cette activite", "voor deze activiteit", "para esta actividad", "per questa attivita" },

    /* STR_SYNC_CONFIG_TITLE */
    { "SYNCHRONIZING CONFIG", "CONFIG SYNCHRONISIEREN", "SYNCHRONISATION CONFIG", "CONFIG SYNCHRONISEREN", "SINCRONIZANDO CONFIG", "SINCRONIZZO CONFIG" },
    /* STR_UPDATING_FROM_HUB */
    { "UPDATING FROM HUB", "AKTUALISIERUNG VOM HUB", "MISE A JOUR DEPUIS HUB", "BIJWERKEN VAN HUB", "ACTUALIZANDO DESDE HUB", "AGGIORNAMENTO DA HUB" },
    /* STR_SYNCING_ACTS_MAPS */
    { "Syncing activities & mappings...", "Sync Aktionen & Tasten...", "Sync activites & touches...", "Activiteiten synchroniseren...", "Sincronizando actividades...", "Sincronizzo attivita..." },
    /* STR_SCREEN_LOCKED_SYNC */
    { "Screen locked during sync", "Bildschirm gesperrt bei Sync", "Ecran verrouille pendant sync", "Scherm vergrendeld bij sync", "Pantalla bloqueada al sync", "Schermo bloccato in sync" },

    /* STR_OTA_TITLE */
    { "FIRMWARE OTA UPDATE", "FIRMWARE OTA UPDATE", "MISE A JOUR OTA", "FIRMWARE OTA UPDATE", "ACTUALIZACION FIRMWARE OTA", "AGGIORNAMENTO FIRMWARE OTA" },
    /* STR_UPDATING_FIRMWARE */
    { "UPDATING FIRMWARE", "FIRMWARE WIRD AKTUALISIERT", "MISE A JOUR DU FIRMWARE", "FIRMWARE BIJWERKEN", "ACTUALIZANDO FIRMWARE", "AGGIORNAMENTO FIRMWARE" },
    /* STR_RECEIVING_OVER_RF */
    { "Receiving firmware over RF", "Empfange Firmware ueber RF", "Reception firmware via RF", "Firmware ontvangen via RF", "Recibiendo firmware por RF", "Ricezione firmware via RF" },
    /* STR_DO_NOT_POWER_OFF */
    { "Do not power off remote or dock!", "Nicht ausschalten / abdocken!", "Ne pas eteindre ni retirer!", "Niet uitschakelen / loskoppelen!", "No apagar ni retirar de base!", "Non spegnere o sollevare!" },
    /* STR_KEEP_CLOSE_TO_HUB */
    { "Keep remote close to Hub.", "Fernbedienung nah am Hub halten.", "Garder pres du Hub.", "Houd dicht bij de Hub.", "Mantenga cerca del Hub.", "Tieni vicino all'Hub." },
    /* STR_UPDATE_IN_PROGRESS */
    { "Firmware update in progress", "Firmware-Update laeuft", "Mise a jour en cours", "Firmware-update bezig", "Actualizacion en curso", "Aggiornamento in corso" },

    /* STR_RF_HARDWARE_LINK */
    { "RF HARDWARE & LINK", "RF HARDWARE & STATUS", "MATERIEL RF & LIAISON", "RF HARDWARE & VERBINDING", "HARDWARE RF Y ENLACE", "HARDWARE RF E STATO" },
    /* STR_STATUS_PAIRED */
    { "Status: PAIRED TO HUB (OK)", "Status: GEKOPPELT (OK)", "Statut: APPAREILLE (OK)", "Status: GEKOPPELD (OK)", "Estado: VINCULADO (OK)", "Stato: ABBINATO (OK)" },
    /* STR_STATUS_NOT_PAIRED */
    { "Status: NOT PAIRED", "Status: NICHT GEKOPPELT", "Statut: NON PAIRE", "Status: NIET GEKOPPELD", "Estado: NO VINCULADO", "Stato: NON ABBINATO" },
    /* STR_STATUS_PAIRING */
    { "Status: PAIRING IN PROGRESS...", "Status: KOPPLUNG LAEUFT...", "Statut: PAIRAGE EN COURS...", "Status: KOPPELEN BEZIG...", "Estado: VINCULANDO...", "Stato: ABBINAMENTO IN CORSO..." },
    /* STR_STATUS_FAILED */
    { "Status: PAIRING FAILED / TIMEOUT", "Status: KOPPLUNG FEHLGESCHLAGEN", "Statut: ECHEC DU PAIRAGE", "Status: KOPPELEN MISLUKT", "Estado: ERROR DE VINCULACION", "Stato: ABBINAMENTO FALLITO" },
    /* STR_UNPAIR_CONFIRM */
    { "UNPAIR CONFIRMATION", "ENTKOPPLUNG BESTAETIGEN", "CONFIRMER DISSOCIATION", "ONTKOPPELEN BEVESTIGEN", "CONFIRMAR DESVINCULACION", "CONFERMA DISACCOPPIAMENTO" },
    /* STR_WIPE_BONDING_CONF */
    { "Wipe bonding & local config?", "Kopplung & Config loeschen?", "Effacer liaison et config?", "Koppeling & config wissen?", "Borrar vinculo y config?", "Cancella associazione e config?" },
    /* STR_CONFIRM_UNPAIR */
    { "Confirm Unpair & Wipe", "Entkoppeln & Loeschen", "Confirmer et effacer", "Ontkoppelen & wissen", "Confirmar desvincular", "Conferma e dissocia" },
    /* STR_CANCEL */
    { "Cancel", "Abbrechen", "Annuler", "Annuleren", "Cancelar", "Annulla" },
    /* STR_STOP_PAIRING */
    { "Stop Pairing", "Kopplung stoppen", "Arreter pairage", "Koppelen stoppen", "Detener vinculacion", "Interrompi abbinamento" },
    /* STR_UNPAIR_FROM_HUB */
    { "Unpair from Hub", "Vom Hub trennen", "Dissocier du Hub", "Ontkoppelen van Hub", "Desvincular del Hub", "Disassocia dall'Hub" },
    /* STR_START_PAIRING */
    { "Start Pairing to Hub", "Mit Hub koppeln", "Lancer le pairage", "Koppelen met Hub", "Iniciar vinculacion", "Avvia abbinamento" },
    /* STR_REMOTE_NOT_PAIRED */
    { "REMOTE NOT PAIRED", "NICHT GEKOPPELT", "NON APPAREILLEE", "NIET GEKOPPELD", "MANDO NO VINCULADO", "TELECOMANDO NON ABBINATO" },
    /* STR_CONNECT_WITH_HUB */
    { "Connect with Harmony Hub", "Mit Harmony Hub verbinden", "Connecter au Harmony Hub", "Verbind met Harmony Hub", "Conectar con Harmony Hub", "Connetti con Harmony Hub" },
    /* STR_CONTROL_ACTS_DEVS */
    { "to control activities & devices.", "fuer Aktionen und Geraete.", "pour gerer les activites.", "voor activiteiten & apparaten.", "para controlar dispositivos.", "per controllare dispositivi." },
    /* STR_TAP_BUTTON_TO_PAIR */
    { "Tap button below to pair:", "Unten tippen zum Koppeln:", "Toucher ci-dessous pour pairer:", "Tik hieronder om te koppelen:", "Toque abajo para vincular:", "Tocca sotto per abbinare:" },
    /* STR_PAIR_TO_HUB */
    { "PAIR TO HUB", "MIT HUB KOPPELN", "PAIRER AU HUB", "KOPPELEN MET HUB", "VINCULAR AL HUB", "ABBINA ALL'HUB" },

    /* STR_CAT_CONFIG */
    { "CONFIG", "CONFIG", "CONFIG", "CONFIG", "CONFIG", "CONFIG" },
    /* STR_CAT_CONFIG_DESC */
    { "Confirm switch, language, sync", "Bestaetigen, Sprache, Sync", "Confirmer, langue, sync", "Bevestigen, taal, sync", "Confirmar, idioma, sync", "Conferma, lingua, sync" },
    /* STR_CAT_DISPLAY */
    { "DISPLAY", "DISPLAY", "AFFICHAGE", "SCHERM", "PANTALLA", "SCHERMO" },
    /* STR_CAT_DISPLAY_DESC */
    { "Brightness, timeout, night", "Helligkeit, Timeout, Nacht", "Luminosite, delai, nuit", "Helderheid, time-out, nacht", "Brillo, espera, noche", "Luminosita, timeout, notte" },
    /* STR_CAT_TOUCH */
    { "TOUCH", "TOUCH", "TACTILE", "AANRAKING", "TACTIL", "TOUCH" },
    /* STR_CAT_TOUCH_DESC */
    { "Sensitivity, nav buttons", "Empfindlichkeit, Tasten", "Sensibilite, touches nav", "Gevoeligheid, navigatie", "Sensibilidad, navegacion", "Sensibilita, navigazione" },
    /* STR_CAT_POWER */
    { "POWER", "POWER", "ALIMENTATION", "ENERGIE", "ENERGIA", "ALIMENTAZIONE" },
    /* STR_CAT_POWER_DESC */
    { "Tilt wake, battery, suspend", "Bewegung, Akku, Ruhezustand", "Mouvement, batterie, veille", "Beweging, batterij, slaap", "Giroscopio, bateria, suspender", "Movimento, batteria, sospendi" },
    /* STR_CAT_SYSTEM */
    { "SYSTEM", "SYSTEM", "SYSTEME", "SYSTEEM", "SISTEMA", "SISTEMA" },
    /* STR_CAT_SYSTEM_DESC */
    { "Pairing, reboot, reset", "Pairing, Neustart, Reset", "Pairage, redemarrage, reset", "Koppelen, herstarten, reset", "Vinculo, reiniciar, reset", "Abbinamento, riavvio, reset" },
    /* STR_CAT_UPDATE */
    { "FIRMWARE UPDATE", "FIRMWARE UPDATE", "MISE A JOUR", "FIRMWARE UPDATE", "ACTUALIZAR FIRMWARE", "AGGIORNAMENTO FIRMWARE" },
    /* STR_CAT_UPDATE_DESC */
    { "Software update & OTA transfer", "Software-Update & OTA-Sync", "Mise a jour & transfert OTA", "Software-update & OTA", "Actualizacion & OTA", "Aggiornamento e OTA" },

    /* STR_CONFIRM_SWITCH */
    { "Confirm Switch", "Umschalten bestaetigen", "Confirmer changement", "Wisselen bevestigen", "Confirmar cambio", "Conferma cambio" },
    /* STR_CONFIRM_PROMPT */
    { "PROMPT", "FRAGEN", "CONFIRMER", "VRAGEN", "AVISO", "CHIEDI" },
    /* STR_CONFIRM_INSTANT */
    { "INSTANT", "SOFORT", "DIRECT", "DIRECT", "DIRECTO", "SUBITO" },
    /* STR_LANGUAGE */
    { "Language", "Sprache", "Langue", "Taal", "Idioma", "Lingua" },
    /* STR_SYNC_CONFIG_HUB */
    { "Sync Config from Hub", "Config vom Hub holen", "Sync config depuis Hub", "Config van Hub ophalen", "Sincronizar del Hub", "Sincronizza da Hub" },

    /* STR_DISPLAY_BRIGHTNESS */
    { "Display Brightness", "Display-Helligkeit", "Luminosite ecran", "Schermhelderheid", "Brillo de pantalla", "Luminosita schermo" },
    /* STR_DISPLAY_TIMEOUT */
    { "Display Timeout", "Display-Timeout", "Delai d'extinction", "Scherm time-out", "Tiempo de espera", "Timeout schermo" },
    /* STR_BTN_BACKLIGHT */
    { "Button Backlight", "Tastenbeleuchtung", "Eclairage touches", "Knopverlichting", "Luz de botones", "Retroilluminazione tasti" },
    /* STR_NIGHT_START */
    { "Night Starts", "Nacht beginnt", "Debut nuit", "Nacht begint", "Inicio noche", "Inizio notte" },
    /* STR_NIGHT_END */
    { "Night Ends", "Nacht endet", "Fin nuit", "Nacht eindigt", "Fin noche", "Fine notte" },
    /* STR_DISPLAY_THEME */
    { "Theme", "Design", "Theme", "Thema", "Tema", "Tema" },
    /* STR_HOME_LAYOUT */
    { "Home Layout", "Startseite", "Accueil", "Startscherm", "Inicio", "Home" },
    /* STR_ACTIVITY_ORDER */
    { "Activity Order", "Reihenfolge", "Ordre", "Volgorde", "Orden", "Ordine" },

    /* STR_TOUCHSCREEN */
    { "Touchscreen", "Touchscreen", "Ecran tactile", "Touchscreen", "Pantalla tactil", "Touchscreen" },
    /* STR_CAP_BUTTONS */
    { "Capacitive Buttons", "Sensortasten", "Touches capacitives", "Sensorknoppen", "Botones capacitivos", "Tasti capacitivi" },
    /* STR_TOUCH_SENSITIVITY */
    { "Touch Sensitivity", "Touch-Empfindlichkeit", "Sensibilite tactile", "Aanraakgevoeligheid", "Sensibilidad tactil", "Sensibilita tocco" },
    /* STR_PHYSICAL_NAV */
    { "Physical Button Nav", "Physische Tasten-Nav", "Nav par touches", "Fysieke knoppennav.", "Nav botones fisicos", "Nav tasti fisici" },

    /* STR_TILT_WAKE */
    { "Tilt / Pickup Wake", "Beim Anheben wecken", "Reveil au mouvement", "Ontwaken bij optillen", "Encender al mover", "Risveglio al movimento" },
    /* STR_GYRO_RATE */
    { "Gyroscope Rate", "Gyroskop-Rate", "Frequence gyro", "Gyroscoopsnelheid", "Tasa giroscopio", "Frequenza giroscopio" },
    /* STR_DEEP_STANDBY */
    { "Deep Standby", "Tiefschlaf", "Veille profonde", "Diepe slaapstand", "Suspension profunda", "Sospensione profonda" },
    /* STR_BATTERY_LEVEL */
    { "Battery Level", "Akkustand", "Niveau batterie", "Batterijniveau", "Nivel de bateria", "Livello batteria" },
    /* STR_BATTERY_VOLTAGE */
    { "Battery Voltage", "Akkuspannung", "Tension batterie", "Batterijspanning", "Voltaje de bateria", "Tensione batteria" },
    /* STR_SUSPEND_RAM */
    { "Suspend to RAM", "Ruhezustand (RAM)", "Mettre en veille (RAM)", "Slaapstand (RAM)", "Suspender a RAM", "Sospendi in RAM" },

    /* STR_USB_ETHERNET */
    { "USB Ethernet", "USB-Ethernet", "Ethernet USB", "USB-ethernet", "Ethernet USB", "Ethernet USB" },
    /* STR_DEBUG_LOGGING */
    { "Debug Logging", "Debug-Protokoll", "Journal debogage", "Debug-logboek", "Registro depuracion", "Log di debug" },
    /* STR_RF_PAIRING_LINK */
    { "RF Pairing & Link", "RF-Kopplung & Status", "Pairage RF & Statut", "RF-koppeling & status", "Vinculo RF y enlace", "Abbinamento RF & stato" },
    /* STR_REBOOT_REMOTE */
    { "Reboot Remote", "Fernbedienung neustarten", "Redemarrer telecommande", "Afstandsbediening herstarten", "Reiniciar mando", "Riavvia telecomando" },
    /* STR_FACTORY_RESET */
    { "Factory Reset", "Auf Werkseinstellungen", "Reinitialisation usine", "Fabrieksreset", "Restablecer fabrica", "Ripristino fabbrica" },

    /* STR_PULL_FIRMWARE_OTA */
    { "Pull Firmware OTA from Hub", "OTA-Firmware vom Hub laden", "Telecharger OTA depuis Hub", "Firmware OTA van Hub laden", "Descargar OTA desde Hub", "Scarica OTA da Hub" },
    /* STR_CHECK_UPDATES */
    { "Check for Updates", "Nach Updates suchen", "Verifier mises a jour", "Zoeken naar updates", "Buscar actualizaciones", "Verifica aggiornamenti" },

    /* STR_CANCEL_KEEP_SYSTEM */
    { "CANCEL - KEEP SYSTEM", "ABBRECHEN - BEIBEHALTEN", "ANNULER - GARDER SYSTEME", "ANNULEREN - BEHOUDEN", "CANCELAR - MANTENER", "ANNULLA - MANTIENI" },
    /* STR_PROCEED_TO_RESET */
    { "PROCEED TO RESET >", "WEITER ZUM RESET >", "CONTINUER RESET >", "VERDER MET RESET >", "CONTINUAR RESET >", "PROCEDI AL RESET >" },
    /* STR_NO_GO_BACK */
    { "NO, GO BACK", "NEIN, ZURUECK", "NON, RETOUR", "NEE, GA TERUG", "NO, VOLVER", "NO, TORNA INDIETRO" },
    /* STR_YES_RESET_REMOTE */
    { "YES, RESET REMOTE NOW", "JA, JETZT ZURUECKSETZEN", "OUI, REINITIALISER", "JA, NU RESETTEN", "SI, REINICIAR AHORA", "SI, RIPRISTINA ORA" },
    /* STR_CANCEL_DO_NOT_REBOOT */
    { "CANCEL - DO NOT REBOOT", "ABBRECHEN - NICHT NEUSTARTEN", "ANNULER - NE PAS REDEMARRER", "ANNULEREN - NIET HERSTARTEN", "CANCELAR - NO REINICIAR", "ANNULLA - NON RIAVVIARE" },
    /* STR_REBOOT_REMOTE_NOW */
    { "REBOOT REMOTE NOW", "JETZT NEUSTARTEN", "REDEMARRER MAINTENANT", "NU HERSTARTEN", "REINICIAR AHORA", "RIAVVIA ORA" },
    /* STR_CANCEL_STAY_AWAKE */
    { "CANCEL - STAY AWAKE", "ABBRECHEN - WACH BLEIBEN", "ANNULER - RESTER ACTIF", "ANNULEREN - WAAK BLIJVEN", "CANCELAR - SEGUIR ACTIVO", "ANNULLA - RESTA ATTIVO" },
    /* STR_SUSPEND_TO_RAM_NOW */
    { "SUSPEND TO RAM NOW", "JETZT IN RUHEZUSTAND", "METTRE EN VEILLE MAINTENANT", "NU NAAR SLAAPSTAND", "SUSPENDER AHORA", "SOSPENDI ORA" },

    /* STR_NOTICE_NAV_MOVED */
    { "Screen nav moved to bottom buttons", "Nav auf untere Tasten gelegt", "Nav deplacee vers touches bas", "Nav naar onderste knoppen", "Nav movida a botones inferiores", "Nav spostata sui tasti inferiori" },
    /* STR_NOTICE_HUB_NOT_CONNECTED */
    { "Hub not connected", "Hub nicht verbunden", "Hub non connecte", "Hub niet verbonden", "Hub no conectado", "Hub non connesso" },
    /* STR_NOTICE_SYNCING */
    { "Syncing config from Hub...", "Sync mit Hub...", "Sync depuis le Hub...", "Syncen vanaf Hub...", "Sincronizando del Hub...", "Sincronizzazione da Hub..." },
    /* STR_NOTICE_REBOOTING */
    { "Rebooting OS...", "Betriebssystem startet neu...", "Redemarrage du systeme...", "Systeem herstarten...", "Reiniciando SO...", "Riavvio del sistema..." },
    /* STR_NOTICE_RESTARTING */
    { "Restarting Remote...", "Fernbedienung startet neu...", "Redemarrage telecommande...", "Afstandsbediening herstart...", "Reiniciando mando...", "Riavvio telecomando..." },
    /* STR_NOTICE_RESTORING */
    { "Restoring stock & rebooting...", "Werkseinstellung & Neustart...", "Restauration Logitech...", "Herstellen en herstarten...", "Restaurando y reiniciando...", "Ripristino e riavvio..." },
    /* STR_NOTICE_LANG_CHANGED */
    { "Language changed", "Sprache geaendert", "Langue modifiee", "Taal gewijzigd", "Idioma cambiado", "Lingua modificata" },
    /* STR_KEY_HINT_TITLE */
    { "Navigation moved", "Navigation geaendert", "Navigation deplacee", "Navigatie verplaatst", "Navegacion movida", "Navigazione spostata" },
    /* STR_KEY_HINT_ACTIVITIES */
    { "Activities key: activity list", "Aktivitaeten-Taste: Liste", "Touche Activites: liste", "Activiteiten-toets: lijst", "Tecla Actividades: lista", "Tasto Attivita: elenco" },
    /* STR_KEY_HINT_DEVICES */
    { "Devices key: all devices", "Geraete-Taste: alle Geraete", "Touche Appareils: appareils", "Apparaten-toets: apparaten", "Tecla Dispositivos: todos", "Tasto Dispositivi: tutti" },
    /* STR_KEY_HINT_OFF */
    { "Off key: power off", "Aus-Taste: ausschalten", "Touche Off: eteindre", "Uit-toets: uitschakelen", "Tecla Off: apagar", "Tasto Off: spegni" },
    /* STR_KEY_HINT_MENU */
    { "Gear or hold Activities: menu", "Zahnrad/Halten: Menue", "Engrenage/maintien: menu", "Tandwiel/vasthouden: menu", "Engranaje/mantener: menu", "Ingranaggio/tieni: menu" },
    /* STR_KEY_HINT_DISMISS */
    { "Press any key to continue", "Taste druecken", "Appuyez sur une touche", "Druk op een toets", "Pulse una tecla", "Premi un tasto" },
    /* STR_HUB_SILENT */
    { "HUB?", "HUB?", "HUB?", "HUB?", "HUB?", "HUB?" },
    /* STR_LINK_TITLE */
    { "RF link to the Hub", "Funk zum Hub", "Liaison radio au Hub", "Radioverbinding met Hub", "Enlace de radio al Hub", "Collegamento radio all'Hub" },
    /* STR_LINK_UP */
    { "Link up", "Verbunden", "Liaison active", "Verbonden", "Enlace activo", "Collegato" },
    /* STR_LINK_REPLIES */
    { "Hub replies", "Hub-Antworten", "Reponses du Hub", "Hub-antwoorden", "Respuestas del Hub", "Risposte dell'Hub" },
    /* STR_LINK_LAST_FRAME */
    { "Last from Hub", "Zuletzt vom Hub", "Dernier du Hub", "Laatst van Hub", "Ultimo del Hub", "Ultimo dall'Hub" },
    /* STR_LINK_LOST */
    { "Link lost", "Verbindung verloren", "Liaison perdue", "Verbinding verloren", "Enlace perdido", "Collegamento perso" },
    /* STR_LINK_HUB_SILENT */
    { "Hub is not answering", "Hub antwortet nicht", "Le Hub ne repond pas", "Hub antwoordt niet", "El Hub no responde", "L'Hub non risponde" },
    /* STR_HELP */
    { "Help", "Hilfe", "Aide", "Help", "Ayuda", "Aiuto" },
    /* STR_HELP_RESENDING */
    { "Re-sending commands...", "Sende Befehle erneut...", "Renvoi des commandes...", "Opdrachten opnieuw...", "Reenviando comandos...", "Reinvio dei comandi..." },
    /* STR_HELP_IS_IT_ON */
    { "Is it on?", "Ist es an?", "Est-il allume ?", "Staat het aan?", "Esta encendido?", "E acceso?" },
    /* STR_HELP_DEVICE_N_OF */
    { "Device %d of %d", "Geraet %d von %d", "Appareil %d sur %d", "Apparaat %d van %d", "Dispositivo %d de %d", "Dispositivo %d di %d" },
    /* STR_YES */
    { "Yes", "Ja", "Oui", "Ja", "Si", "Si" },
    /* STR_NO */
    { "No", "Nein", "Non", "Nee", "No", "No" },
    /* STR_HELP_SWITCHED_ON */
    { "Switched on: %d", "Eingeschaltet: %d", "Allumes : %d", "Aangezet: %d", "Encendidos: %d", "Accesi: %d" },
    /* STR_HELP_ALL_ON */
    { "Everything should be on", "Alles sollte an sein", "Tout devrait etre allume", "Alles zou aan moeten staan", "Todo deberia estar encendido", "Tutto dovrebbe essere acceso" },
    /* STR_HELP_BUSY */
    { "Hub is busy, try again", "Hub beschaeftigt", "Hub occupe, reessayez", "Hub bezig, probeer opnieuw", "Hub ocupado, reintente", "Hub occupato, riprova" },
    /* STR_HELP_FAILED */
    { "Help could not run", "Hilfe nicht moeglich", "Aide impossible", "Help kon niet starten", "No se pudo ejecutar", "Aiuto non disponibile" },
    /* STR_HELP_BACK_CLOSES */
    { "Back: close", "Zurueck: schliessen", "Retour : fermer", "Terug: sluiten", "Atras: cerrar", "Indietro: chiudi" },
    /* STR_CHARGING */
    { "Charging", "Wird geladen", "En charge", "Opladen", "Cargando", "In carica" },
};

const char *tr_lang(remote_str_id_t id, remote_lang_t lang) {
    if (id < 0 || id >= STR_COUNT) return "";
    if (lang < 0 || lang >= REMOTE_LANG_COUNT) lang = REMOTE_LANG_EN;
    const char *s = g_remote_strings[id][lang];
    if (!s || !s[0]) s = g_remote_strings[id][REMOTE_LANG_EN];
    return s ? s : "";
}

const char *tr(remote_str_id_t id) {
    return tr_lang(id, g_language);
}

const char *remote_lang_code(remote_lang_t lang) {
    switch (lang) {
        case REMOTE_LANG_DE: return "de";
        case REMOTE_LANG_FR: return "fr";
        case REMOTE_LANG_NL: return "nl";
        case REMOTE_LANG_ES: return "es";
        case REMOTE_LANG_IT: return "it";
        case REMOTE_LANG_EN:
        default:            return "en";
    }
}

const char *remote_lang_name(remote_lang_t lang) {
    switch (lang) {
        case REMOTE_LANG_DE: return "Deutsch";
        case REMOTE_LANG_FR: return "Francais";
        case REMOTE_LANG_NL: return "Nederlands";
        case REMOTE_LANG_ES: return "Espanol";
        case REMOTE_LANG_IT: return "Italiano";
        case REMOTE_LANG_EN:
        default:            return "English";
    }
}

remote_lang_t remote_lang_from_code(const char *code) {
    if (!code) return REMOTE_LANG_EN;
    if (strcmp(code, "de") == 0 || strcmp(code, "DE") == 0 || strcmp(code, "1") == 0) return REMOTE_LANG_DE;
    if (strcmp(code, "fr") == 0 || strcmp(code, "FR") == 0 || strcmp(code, "2") == 0) return REMOTE_LANG_FR;
    if (strcmp(code, "nl") == 0 || strcmp(code, "NL") == 0 || strcmp(code, "3") == 0) return REMOTE_LANG_NL;
    if (strcmp(code, "es") == 0 || strcmp(code, "ES") == 0 || strcmp(code, "4") == 0) return REMOTE_LANG_ES;
    if (strcmp(code, "it") == 0 || strcmp(code, "IT") == 0 || strcmp(code, "5") == 0) return REMOTE_LANG_IT;
    return REMOTE_LANG_EN;
}

void remote_lang_cycle(void) {
    g_language = (remote_lang_t)((g_language + 1) % REMOTE_LANG_COUNT);
}
