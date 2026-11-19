"""The port's own in-game texts in the game's languages: one table for the menus the port builds
(scripts/online_mode.py, the Pack Tacho options page) and for the runtime (src/runtime_text.c,
whose table src/runtime_texts.inc is generated from this one by tools/gen_octr_texts.py).

Keys are the English texts as the port writes them. Columns: the languages the two editions have
(CUSA13795: English, Latin American Spanish, French, Brazilian Portuguese; CUSA14876: English,
Spain's Spanish, French, German, Italian, Arabic). Arabic keeps English. A text with no entry, or a
language with no column, stays as written. Where the game has its own word (Copa, Batalla...),
the table uses it.

Usage: python scripts/octr_texts.py --check  (the generated C table matches this one)
SPDX-License-Identifier: GPL-2.0-or-later
"""
import os
import sys

# The table's language columns, in this order.
LANGUAGES = ('mx', 'es', 'fr', 'pt', 'de', 'it')

# PS4 system language ids (BB_LANGUAGE) -> column; anything else is English.
_BY_ID = {20: 'mx', 3: 'es', 2: 'fr', 22: 'fr', 17: 'pt', 7: 'pt', 4: 'de', 5: 'it'}
# The game's own text tables (ui/<project>_<suffix>.lng) -> column.
_BY_SUFFIX = {'mx': 'mx', 'es': 'es', 'fr': 'fr', 'pt': 'pt', 'de': 'de', 'it': 'it'}

S = None  # Spain's Spanish: the same as Latin America's

# (English, Latin American Spanish, Spain's Spanish (S: the same), French, Portuguese, German, Italian)
TEXTS = (
    # ---- Online mode screen and private room settings
    ('GAME MODE', 'MODO DE JUEGO', S, 'MODE DE JEU', 'MODO DE JOGO', 'SPIELMODUS', 'MODALITÀ'),
    ('RACE TYPE', 'TIPO DE CARRERA', S, 'TYPE DE COURSE', 'TIPO DE CORRIDA', 'RENNTYP', 'TIPO DI GARA'),
    ('SINGLE', 'INDIVIDUAL', S, 'SIMPLE', 'ÚNICA', 'EINZELN', 'SINGOLA'),
    ('CUP', 'COPA', S, 'COUPE', 'COPA', 'POKAL', 'COPPA'),
    ('NORMAL', 'NORMAL', S, 'NORMAL', 'NORMAL', 'NORMAL', 'NORMALE'),
    ('ITEMLESS', 'SIN ÍTEMS', 'SIN OBJETOS', 'SANS OBJETS', 'SEM ITENS', 'OHNE ITEMS', 'SENZA OGGETTI'),
    ('BATTLE', 'BATALLA', S, 'BATAILLE', 'BATALHA', 'KAMPF', 'BATTAGLIA'),
    ('N.SANE MODE', 'MODO N.SANE', S, 'MODE N.SANE', 'MODO N.SANE', 'N.SANE-MODUS', 'MODALITÀ N.SANE'),
    ('COLLISION', 'COLISIONES', S, 'COLLISIONS', 'COLISÕES', 'KOLLISIONEN', 'COLLISIONI'),
    ('OFF', 'NO', S, 'NON', 'NÃO', 'AUS', 'NO'),
    ('ON', 'SÍ', S, 'OUI', 'SIM', 'AN', 'SÌ'),
    ('RANKED', 'CLASIFICATORIA', S, 'CLASSÉE', 'RANQUEADA', 'RANGLISTE', 'CLASSIFICATA'),
    ('HALL OF FAME', 'SALÓN DE LA FAMA', S, 'PANTHÉON', 'HALL DA FAMA', 'RUHMESHALLE', "ALBO D'ORO"),
    ('GO TO LOBBY', 'IR A LA SALA', S, 'ALLER AU SALON', 'IR PARA A SALA', 'ZUR LOBBY', 'VAI ALLA LOBBY'),
    ('SELECT CUP', 'ELIGE UNA COPA', S, 'CHOISIS UNE COUPE', 'ESCOLHA UMA COPA', 'POKAL WÄHLEN', 'SCEGLI UNA COPPA'),
    ('LOADING...', 'CARGANDO...', S, 'CHARGEMENT...', 'CARREGANDO...', 'LADEN...', 'CARICAMENTO...'),
    ('READY', 'LISTO', S, 'PRÊT', 'PRONTO', 'BEREIT', 'PRONTO'),
    ('UNREADY', 'NO LISTO', S, 'PAS PRÊT', 'NÃO PRONTO', 'NICHT BEREIT', 'NON PRONTO'),
    ('REFRESH', 'ACTUALIZAR', S, 'ACTUALISER', 'ATUALIZAR', 'AKTUALISIEREN', 'AGGIORNA'),
    # the room card's mode line
    ('SINGLE RACE - NORMAL', 'CARRERA INDIVIDUAL - NORMAL', S, 'COURSE SIMPLE - NORMAL',
     'CORRIDA ÚNICA - NORMAL', 'EINZELRENNEN - NORMAL', 'GARA SINGOLA - NORMALE'),
    ('SINGLE RACE - ITEMLESS', 'CARRERA INDIVIDUAL - SIN ÍTEMS', 'CARRERA INDIVIDUAL - SIN OBJETOS',
     'COURSE SIMPLE - SANS OBJETS', 'CORRIDA ÚNICA - SEM ITENS', 'EINZELRENNEN - OHNE ITEMS',
     'GARA SINGOLA - SENZA OGGETTI'),
    ('SINGLE RACE - N.SANE MODE', 'CARRERA INDIVIDUAL - MODO N.SANE', S, 'COURSE SIMPLE - MODE N.SANE',
     'CORRIDA ÚNICA - MODO N.SANE', 'EINZELRENNEN - N.SANE-MODUS', 'GARA SINGOLA - MODALITÀ N.SANE'),
    ('CUP RACE - NORMAL', 'COPA - NORMAL', S, 'COUPE - NORMAL', 'COPA - NORMAL', 'POKAL - NORMAL',
     'COPPA - NORMALE'),
    ('CUP RACE - ITEMLESS', 'COPA - SIN ÍTEMS', 'COPA - SIN OBJETOS', 'COUPE - SANS OBJETS',
     'COPA - SEM ITENS', 'POKAL - OHNE ITEMS', 'COPPA - SENZA OGGETTI'),
    ('CUP RACE - N.SANE MODE', 'COPA - MODO N.SANE', S, 'COUPE - MODE N.SANE', 'COPA - MODO N.SANE',
     'POKAL - N.SANE-MODUS', 'COPPA - MODALITÀ N.SANE'),
    ('RANKED - NORMAL', 'CLASIFICATORIA - NORMAL', S, 'CLASSÉE - NORMAL', 'RANQUEADA - NORMAL',
     'RANGLISTE - NORMAL', 'CLASSIFICATA - NORMALE'),
    ('RANKED - ITEMLESS', 'CLASIFICATORIA - SIN ÍTEMS', 'CLASIFICATORIA - SIN OBJETOS', 'CLASSÉE - SANS OBJETS',
     'RANQUEADA - SEM ITENS', 'RANGLISTE - OHNE ITEMS', 'CLASSIFICATA - SENZA OGGETTI'),
    # ---- Teams
    ('RESULT', 'RESULTADO', S, 'RÉSULTAT', 'RESULTADO', 'ERGEBNIS', 'RISULTATO'),
    ('WINS', 'VICTORIA', S, 'VICTOIRE', 'VITÓRIA', 'SIEG', 'VITTORIA'),
    ('LOSE', 'DERROTA', S, 'DÉFAITE', 'DERROTA', 'NIEDERLAGE', 'SCONFITTA'),
    ('NO FINISHERS', 'NADIE LLEGÓ A LA META', S, "PERSONNE N'A FINI", 'NINGUÉM TERMINOU', 'NIEMAND IM ZIEL',
     'NESSUNO AL TRAGUARDO'),
    ('%d POINTS%s', '%d PUNTOS%s', S, '%d POINTS%s', '%d PONTOS%s', '%d PUNKTE%s', '%d PUNTI%s'),
    (' (H2H)', ' (CARA A CARA)', S, ' (FACE-À-FACE)', ' (CONFRONTO DIRETO)', ' (DIREKTER VERGLEICH)',
     ' (SCONTRI DIRETTI)'),
    ('2 TEAMS WITH NO AI NEED 2, 4, 6 OR 8 PLAYERS.\nCURRENT PLAYERS: %d\nADJUST PLAYER COUNT OR ENABLE AI.',
     '2 EQUIPOS SIN IA NECESITAN 2, 4, 6 U 8 JUGADORES.\nJUGADORES ACTUALES: %d\nAJUSTA LOS JUGADORES O ACTIVA LA IA.',
     S,
     '2 ÉQUIPES SANS IA DEMANDENT 2, 4, 6 OU 8 JOUEURS.\nJOUEURS ACTUELS : %d\nAJUSTE LES JOUEURS OU ACTIVE L\'IA.',
     '2 EQUIPES SEM IA PRECISAM DE 2, 4, 6 OU 8 JOGADORES.\nJOGADORES ATUAIS: %d\nAJUSTE OS JOGADORES OU ATIVE A IA.',
     '2 TEAMS OHNE KI BRAUCHEN 2, 4, 6 ODER 8 SPIELER.\nAKTUELLE SPIELER: %d\nPASSE DIE SPIELERZAHL AN ODER AKTIVIERE DIE KI.',
     '2 SQUADRE SENZA IA RICHIEDONO 2, 4, 6 O 8 GIOCATORI.\nGIOCATORI ATTUALI: %d\nCAMBIA I GIOCATORI O ATTIVA L\'IA.'),
    ('4 TEAMS WITH NO AI NEED 4 OR 8 PLAYERS.\nCURRENT PLAYERS: %d\nADJUST PLAYER COUNT OR ENABLE AI.',
     '4 EQUIPOS SIN IA NECESITAN 4 U 8 JUGADORES.\nJUGADORES ACTUALES: %d\nAJUSTA LOS JUGADORES O ACTIVA LA IA.',
     S,
     '4 ÉQUIPES SANS IA DEMANDENT 4 OU 8 JOUEURS.\nJOUEURS ACTUELS : %d\nAJUSTE LES JOUEURS OU ACTIVE L\'IA.',
     '4 EQUIPES SEM IA PRECISAM DE 4 OU 8 JOGADORES.\nJOGADORES ATUAIS: %d\nAJUSTE OS JOGADORES OU ATIVE A IA.',
     '4 TEAMS OHNE KI BRAUCHEN 4 ODER 8 SPIELER.\nAKTUELLE SPIELER: %d\nPASSE DIE SPIELERZAHL AN ODER AKTIVIERE DIE KI.',
     '4 SQUADRE SENZA IA RICHIEDONO 4 O 8 GIOCATORI.\nGIOCATORI ATTUALI: %d\nCAMBIA I GIOCATORI O ATTIVA L\'IA.'),
    ('BLUE TEAM WINS','GANA EL EQUIPO AZUL', S, "L'ÉQUIPE BLEUE GAGNE", 'A EQUIPE AZUL VENCE',
     'TEAM BLAU GEWINNT', 'VINCE LA SQUADRA BLU'),
    ('RED TEAM WINS', 'GANA EL EQUIPO ROJO', S, "L'ÉQUIPE ROUGE GAGNE", 'A EQUIPE VERMELHA VENCE',
     'TEAM ROT GEWINNT', 'VINCE LA SQUADRA ROSSA'),
    ('GREEN TEAM WINS', 'GANA EL EQUIPO VERDE', S, "L'ÉQUIPE VERTE GAGNE", 'A EQUIPE VERDE VENCE',
     'TEAM GRÜN GEWINNT', 'VINCE LA SQUADRA VERDE'),
    ('YELLOW TEAM WINS', 'GANA EL EQUIPO AMARILLO', S, "L'ÉQUIPE JAUNE GAGNE", 'A EQUIPE AMARELA VENCE',
     'TEAM GELB GEWINNT', 'VINCE LA SQUADRA GIALLA'),
    # ---- Ranked: searching, results, rank names
    ('SEARCHING', 'BUSCANDO', S, 'RECHERCHE', 'PROCURANDO', 'SUCHE', 'RICERCA'),
    ('ALL RANKS', 'TODOS LOS RANGOS', S, 'TOUS LES RANGS', 'TODOS OS RANQUES', 'ALLE RÄNGE', 'TUTTI I GRADI'),
    ('WIDENED: %d MMR', 'AMPLIADO: %d MMR', S, 'ÉLARGI : %d MMR', 'AMPLIADO: %d MMR', 'ERWEITERT: %d MMR',
     'AMPLIATO: %d MMR'),
    ('RANGE: %d MMR', 'RANGO: %d MMR', S, 'ÉCART : %d MMR', 'FAIXA: %d MMR', 'BEREICH: %d MMR',
     'INTERVALLO: %d MMR'),
    ('CHECKING RANGE', 'REVISANDO EL RANGO', S, "VÉRIFICATION DE L'ÉCART", 'VERIFICANDO A FAIXA',
     'BEREICH WIRD GEPRÜFT', "VERIFICA DELL'INTERVALLO"),
    ('NORMAL RANKED', 'CLASIFICATORIA NORMAL', S, 'CLASSÉE NORMALE', 'RANQUEADA NORMAL', 'RANGLISTE NORMAL',
     'CLASSIFICATA NORMALE'),
    ('ITEMLESS RANKED', 'CLASIFICATORIA SIN ÍTEMS', 'CLASIFICATORIA SIN OBJETOS', 'CLASSÉE SANS OBJETS',
     'RANQUEADA SEM ITENS', 'RANGLISTE OHNE ITEMS', 'CLASSIFICATA SENZA OGGETTI'),
    ('CALCULATING', 'CALCULANDO', S, 'CALCUL EN COURS', 'CALCULANDO', 'WIRD BERECHNET', 'CALCOLO IN CORSO'),
    ('TOTAL %d MMR', 'TOTAL %d MMR', S, 'TOTAL %d MMR', 'TOTAL %d MMR', 'GESAMT %d MMR', 'TOTALE %d MMR'),
    ('PLACEMENT RACES', 'CARRERAS DE POSICIONAMIENTO', S, 'COURSES DE PLACEMENT', 'CORRIDAS DE POSICIONAMENTO',
     'PLATZIERUNGSRENNEN', 'GARE DI PIAZZAMENTO'),
    ('UNRANKED', 'SIN RANGO', S, 'NON CLASSÉ', 'SEM RANQUE', 'OHNE RANG', 'SENZA GRADO'),
    # Normal (FFA) tiers since 2026-11-18 (icons and names by S_97)
    ('Bronze', 'Bronce', S, 'Bronze', 'Bronze', 'Bronze', 'Bronzo'),
    ('Silver', 'Plata', S, 'Argent', 'Prata', 'Silber', 'Argento'),
    ('Golden Wumpa', 'Wumpa Dorada', S, 'Wumpa doré', 'Wumpa Dourada', 'Goldene Wumpa', "Wumpa d'oro"),
    ('Power Crystal', 'Cristal de Poder', S, 'Cristal de pouvoir', 'Cristal de Poder', 'Kraftkristall',
     'Cristallo del potere'),
    ('Mythic Gem', 'Gema Mítica', S, 'Gemme mythique', 'Gema Mítica', 'Mythisches Juwel', 'Gemma mitica'),
    ("Uka's Chosen", 'Elegido de Uka', S, "Élu d'Uka", 'Escolhido de Uka', 'Ukas Auserwählter', 'Prescelto di Uka'),
    ('Ancient Spirit', 'Espíritu Ancestral', S, 'Esprit ancestral', 'Espírito Ancestral', 'Uralter Geist',
     'Spirito ancestrale'),
    ('Sapphire Relic', 'Reliquia de Zafiro', S, 'Relique de saphir', 'Relíquia de Safira', 'Saphir-Relikt',
     'Reliquia di zaffiro'),
    ('Gold Relic', 'Reliquia de Oro', S, "Relique d'or", 'Relíquia de Ouro', 'Gold-Relikt', "Reliquia d'oro"),
    ('Platinum Relic', 'Reliquia de Platino', S, 'Relique de platine', 'Relíquia de Platina', 'Platin-Relikt',
     'Reliquia di platino'),
    ('Sacred Fire', 'Fuego Sagrado', S, 'Feu sacré', 'Fogo Sagrado', 'Heiliges Feuer', 'Fuoco Sacro'),
    ('Ultimate Sacred Fire', 'Fuego Sagrado Supremo', S, 'Feu sacré ultime', 'Fogo Sagrado Supremo',
     'Ultimatives Heiliges Feuer', 'Fuoco Sacro Supremo'),
    ('Time Crusher', 'Triturador del Tiempo', S, 'Broyeur de temps', 'Esmagador do Tempo', 'Zeitbrecher',
     'Frantumatempo'),
    ('Oxide Overlord', 'Soberano Oxide', S, 'Suzerain Oxide', 'Soberano Oxide', 'Oxide-Oberherr',
     'Sovrano Oxide'),
    # ---- Hall of Fame
    ('SEASON', 'TEMPORADA', S, 'SAISON', 'TEMPORADA', 'SAISON', 'STAGIONE'),
    ('SEASON %d', 'TEMPORADA %d', S, 'SAISON %d', 'TEMPORADA %d', 'SAISON %d', 'STAGIONE %d'),
    ('POPULATION', 'JUGADORES', S, 'JOUEURS', 'JOGADORES', 'SPIELER', 'GIOCATORI'),
    ('FRIENDS', 'AMIGOS', S, 'AMIS', 'AMIGOS', 'FREUNDE', 'AMICI'),
    ('EVERYONE', 'TODOS', S, 'TOUT LE MONDE', 'TODOS', 'ALLE', 'TUTTI'),
    ('LEADERBOARD UNAVAILABLE', 'CLASIFICACIÓN NO DISPONIBLE', S, 'CLASSEMENT INDISPONIBLE',
     'CLASSIFICAÇÃO INDISPONÍVEL', 'RANGLISTE NICHT VERFÜGBAR', 'CLASSIFICA NON DISPONIBILE'),
    ('No Data', 'Sin datos', S, 'Aucune donnée', 'Sem dados', 'Keine Daten', 'Nessun dato'),
    # ---- Replays
    ('REPLAY ENDED', 'REPETICIÓN TERMINADA', S, 'REPLAY TERMINÉ', 'REPLAY ENCERRADO', 'WIEDERHOLUNG BEENDET',
     'REPLAY TERMINATO'),
    ('REPLAY PAUSED', 'REPETICIÓN EN PAUSA', S, 'REPLAY EN PAUSE', 'REPLAY PAUSADO', 'WIEDERHOLUNG PAUSIERT',
     'REPLAY IN PAUSA'),
    ('LIVE (WAITING)', 'EN VIVO (ESPERANDO)', 'EN DIRECTO (ESPERANDO)', 'EN DIRECT (EN ATTENTE)',
     'AO VIVO (AGUARDANDO)', 'LIVE (WARTET)', 'IN DIRETTA (IN ATTESA)'),
    ('LIVE', 'EN VIVO', 'EN DIRECTO', 'EN DIRECT', 'AO VIVO', 'LIVE', 'IN DIRETTA'),
    ('REPLAY %s', 'REPETICIÓN %s', S, 'REPLAY %s', 'REPLAY %s', 'WIEDERHOLUNG %s', 'REPLAY %s'),
    ('   FINISHED %d%s', '   LLEGÓ %d%s', S, '   ARRIVÉ %d%s', '   CHEGOU %d%s', '   IM ZIEL %d%s',
     '   ARRIVATO %d%s'),
    # place suffixes (1ST, 2ND...)
    ('ST', 'º', S, 'ER', 'º', '.', 'º'),
    ('ND', 'º', S, 'E', 'º', '.', 'º'),
    ('RD', 'º', S, 'E', 'º', '.', 'º'),
    ('TH', 'º', S, 'E', 'º', '.', 'º'),
    # ---- Graphics tab (Options)
    ('Resolution', 'Resolución', S, 'Résolution', 'Resolução', 'Auflösung', 'Risoluzione'),
    ('Aspect Ratio', 'Relación de aspecto', S, "Format d'image", 'Proporção de tela', 'Seitenverhältnis',
     'Proporzioni'),
    ('Upscaling', 'Escalado', S, "Mise à l'échelle", 'Escalonamento', 'Hochskalierung', 'Upscaling'),
    ('Upscale Quality', 'Calidad de escalado', S, "Qualité de mise à l'échelle", 'Qualidade do escalonamento',
     'Hochskalierungsqualität', "Qualità dell'upscaling"),
    ('FPS Unlock', 'FPS desbloqueados', S, 'FPS débloqués', 'FPS desbloqueado', 'FPS-Freischaltung', 'Sblocco FPS'),
    ('Motion Blur', 'Desenfoque de movimiento', S, 'Flou de mouvement', 'Desfoque de movimento',
     'Bewegungsunschärfe', 'Sfocatura di movimento'),
    ('FPS Limit', 'Límite de FPS', S, 'Limite de FPS', 'Limite de FPS', 'FPS-Limit', 'Limite FPS'),
    ('Off', 'No', S, 'Non', 'Não', 'Aus', 'No'),
    ('On', 'Sí', S, 'Oui', 'Sim', 'An', 'Sì'),
    ('Auto', 'Automático', S, 'Auto', 'Automático', 'Auto', 'Automatico'),
    ('Quality', 'Calidad', S, 'Qualité', 'Qualidade', 'Qualität', 'Qualità'),
    ('Balanced', 'Equilibrado', S, 'Équilibré', 'Equilibrado', 'Ausgewogen', 'Bilanciato'),
    ('Performance', 'Rendimiento', S, 'Performances', 'Desempenho', 'Leistung', 'Prestazioni'),
    ('Ultra Perf.', 'Ultra rend.', S, 'Ultra perf.', 'Ultra desemp.', 'Ultra-Leistung', 'Ultra prest.'),
    ('RESTART TO APPLY', 'REINICIA PARA APLICAR', S, 'REDÉMARRE POUR APPLIQUER', 'REINICIE PARA APLICAR',
     'NEUSTART ZUM ANWENDEN', 'RIAVVIA PER APPLICARE'),
    ('Choose the internal rendering resolution. Applies live to the 3D scene.',
     'Elige la resolución interna de renderizado. Se aplica al instante en la escena 3D.', S,
     'Choisis la résolution de rendu interne. S\'applique immédiatement à la scène 3D.',
     'Escolha a resolução interna de renderização. Aplica na hora à cena 3D.',
     'Wähle die interne Renderauflösung. Wirkt sofort auf die 3D-Szene.',
     'Scegli la risoluzione di rendering interna. Si applica subito alla scena 3D.'),
    ('Choose DLSS image quality. Applies live at the current output size.',
     'Elige la calidad de imagen de DLSS. Se aplica al instante con el tamaño de salida actual.', S,
     "Choisis la qualité d'image DLSS. S'applique immédiatement à la taille de sortie actuelle.",
     'Escolha a qualidade de imagem do DLSS. Aplica na hora no tamanho de saída atual.',
     'Wähle die DLSS-Bildqualität. Wirkt sofort bei der aktuellen Ausgabegröße.',
     "Scegli la qualità d'immagine DLSS. Si applica subito alla dimensione di uscita attuale."),
    ('Unlock the frame rate immediately; Off limits the game to 30 FPS.',
     'Desbloquea los FPS al instante; No limita el juego a 30 FPS.', S,
     "Débloque la fréquence d'images immédiatement ; Non limite le jeu à 30 FPS.",
     'Desbloqueia a taxa de quadros na hora; Não limita o jogo a 30 FPS.',
     'Schaltet die Bildrate sofort frei; Aus begrenzt das Spiel auf 30 FPS.',
     'Sblocca subito il frame rate; No limita il gioco a 30 FPS.'),
    ('Enable or disable motion blur immediately.',
     'Activa o desactiva el desenfoque de movimiento al instante.', S,
     'Active ou désactive le flou de mouvement immédiatement.',
     'Ativa ou desativa o desfoque de movimento na hora.',
     'Schaltet die Bewegungsunschärfe sofort ein oder aus.',
     'Attiva o disattiva subito la sfocatura di movimento.'),
    ('Choose the internal rendering resolution. Restart to apply.',
     'Elige la resolución interna de renderizado. Reinicia para aplicar.', S,
     'Choisis la résolution de rendu interne. Redémarre pour appliquer.',
     'Escolha a resolução interna de renderização. Reinicie para aplicar.',
     'Wähle die interne Renderauflösung. Neustart zum Anwenden.',
     'Scegli la risoluzione di rendering interna. Riavvia per applicare.'),
    ('Choose a 16:9, 16:10, 21:9, or 32:9 layout. Restart to apply.',
     'Elige un formato 16:9, 16:10, 21:9 o 32:9. Reinicia para aplicar.', S,
     'Choisis un format 16:9, 16:10, 21:9 ou 32:9. Redémarre pour appliquer.',
     'Escolha um formato 16:9, 16:10, 21:9 ou 32:9. Reinicie para aplicar.',
     'Wähle 16:9, 16:10, 21:9 oder 32:9. Neustart zum Anwenden.',
     'Scegli un formato 16:9, 16:10, 21:9 o 32:9. Riavvia per applicare.'),
    ('Choose the image upscaler. Applies immediately at the current render size.',
     'Elige el escalador de imagen. Se aplica al instante con el tamaño de renderizado actual.', S,
     "Choisis la mise à l'échelle de l'image. S'applique immédiatement à la taille de rendu actuelle.",
     'Escolha o escalonador de imagem. Aplica na hora no tamanho de renderização atual.',
     'Wähle den Bild-Upscaler. Wirkt sofort bei der aktuellen Rendergröße.',
     "Scegli l'upscaler dell'immagine. Si applica subito alla dimensione di rendering attuale."),
    ('Choose DLSS image quality. A different internal render size requires a restart.',
     'Elige la calidad de imagen de DLSS. Otro tamaño de renderizado interno requiere reiniciar.', S,
     "Choisis la qualité d'image DLSS. Une autre taille de rendu interne nécessite un redémarrage.",
     'Escolha a qualidade de imagem do DLSS. Outro tamanho de renderização interna exige reiniciar.',
     'Wähle die DLSS-Bildqualität. Eine andere interne Rendergröße erfordert einen Neustart.',
     "Scegli la qualità d'immagine DLSS. Un'altra dimensione di rendering interna richiede il riavvio."),
    ("Unlock the game's frame rate. Restart to apply.",
     'Desbloquea los FPS del juego. Reinicia para aplicar.', S,
     "Débloque la fréquence d'images du jeu. Redémarre pour appliquer.",
     'Desbloqueia a taxa de quadros do jogo. Reinicie para aplicar.',
     'Schaltet die Bildrate des Spiels frei. Neustart zum Anwenden.',
     'Sblocca il frame rate del gioco. Riavvia per applicare.'),
    ('Enable or disable motion blur. Restart to apply.',
     'Activa o desactiva el desenfoque de movimiento. Reinicia para aplicar.', S,
     'Active ou désactive le flou de mouvement. Redémarre pour appliquer.',
     'Ativa ou desativa o desfoque de movimento. Reinicie para aplicar.',
     'Schaltet die Bewegungsunschärfe ein oder aus. Neustart zum Anwenden.',
     'Attiva o disattiva la sfocatura di movimento. Riavvia per applicare.'),
    ('Limit the number of frames rendered per second. Applies immediately.',
     'Limita los fotogramas por segundo. Se aplica al instante.', S,
     "Limite le nombre d'images par seconde. S'applique immédiatement.",
     'Limita os quadros por segundo. Aplica na hora.',
     'Begrenzt die Bilder pro Sekunde. Wirkt sofort.',
     'Limita i fotogrammi al secondo. Si applica subito.'),
    ('Restart the game to apply your saved graphics settings.',
     'Reinicia el juego para aplicar tus ajustes gráficos guardados.', S,
     'Redémarre le jeu pour appliquer tes réglages graphiques enregistrés.',
     'Reinicie o jogo para aplicar suas configurações gráficas salvas.',
     'Starte das Spiel neu, um deine gespeicherten Grafikeinstellungen anzuwenden.',
     'Riavvia il gioco per applicare le impostazioni grafiche salvate.'),
    # ---- Pack Tacho's page of Options > Game (its texts are Spanish in the pack)
    ('Reserves bar', 'Barra de reservas', S, 'Barre de réserves', 'Barra de reservas', 'Reservenleiste',
     'Barra delle riserve'),
    ('Yes: shows your boost reserves under the boost bar. No: hides it.',
     'Sí: muestra tus reservas de turbo debajo de la barra de turbo. No: la oculta.', S,
     'Oui : affiche tes réserves de turbo sous la barre de turbo. Non : la masque.',
     'Sim: mostra suas reservas de turbo abaixo da barra de turbo. Não: oculta.',
     'Ja: zeigt deine Turbo-Reserven unter der Turboleiste. Nein: blendet sie aus.',
     'Sì: mostra le tue riserve di turbo sotto la barra del turbo. No: la nasconde.'),
    ('Yes: 8-place list with name, wumpas and item. No: only the top 4, no extras.',
     'Sí: lista de 8 con nombre, wumpas e ítem. No: solo los 4 primeros, sin extras.',
     'Sí: lista de 8 con nombre, wumpas y objeto. No: solo los 4 primeros, sin extras.',
     'Oui : liste de 8 avec nom, wumpas et objet. Non : seulement les 4 premiers, sans extras.',
     'Sim: lista de 8 com nome, wumpas e item. Não: só os 4 primeiros, sem extras.',
     'Ja: Liste mit 8 Plätzen, Name, Wumpas und Item. Nein: nur die ersten 4, ohne Extras.',
     'Sì: lista di 8 con nome, wumpa e oggetto. No: solo i primi 4, senza extra.'),
    # ---- Notices (one OK button)
    ('Failed to initialize network, try again later',
     'No se pudo iniciar la red. Inténtalo más tarde.', S,
     "Impossible d'initialiser le réseau. Réessaie plus tard.",
     'Não foi possível iniciar a rede. Tente mais tarde.',
     'Das Netzwerk konnte nicht gestartet werden. Versuche es später erneut.',
     'Impossibile avviare la rete. Riprova più tardi.'),
    ('Connection timed out. Please try Online again.',
     'Se agotó el tiempo de conexión. Vuelve a entrar al modo en línea.', S,
     'La connexion a expiré. Réessaie le mode en ligne.',
     'A conexão expirou. Tente o modo online de novo.',
     'Zeitüberschreitung der Verbindung. Versuche den Online-Modus erneut.',
     'Connessione scaduta. Riprova la modalità online.'),
    ("Cannot synchronize the host's cup. Return to the lobby and retry.",
     'No se pudo sincronizar la copa del anfitrión. Vuelve a la sala e inténtalo de nuevo.', S,
     "Impossible de synchroniser la coupe de l'hôte. Retourne au salon et réessaie.",
     'Não foi possível sincronizar a copa do anfitrião. Volte para a sala e tente de novo.',
     'Der Pokal des Hosts konnte nicht synchronisiert werden. Kehre zur Lobby zurück und versuche es erneut.',
     "Impossibile sincronizzare la coppa dell'host. Torna alla lobby e riprova."),
    ("Cannot synchronize the host's race mode. Return to the lobby and retry.",
     'No se pudo sincronizar el modo de carrera del anfitrión. Vuelve a la sala e inténtalo de nuevo.', S,
     "Impossible de synchroniser le mode de course de l'hôte. Retourne au salon et réessaie.",
     'Não foi possível sincronizar o modo de corrida do anfitrião. Volte para a sala e tente de novo.',
     'Der Rennmodus des Hosts konnte nicht synchronisiert werden. Kehre zur Lobby zurück und versuche es erneut.',
     "Impossibile sincronizzare la modalità di gara dell'host. Torna alla lobby e riprova."),
    ('Room settings are still synchronizing. Please retry Play.',
     'La configuración de la sala aún se está sincronizando. Vuelve a pulsar Jugar.', S,
     'Les réglages du salon sont encore en cours de synchronisation. Réessaie Jouer.',
     'As configurações da sala ainda estão sincronizando. Tente Jogar de novo.',
     'Die Raumeinstellungen werden noch synchronisiert. Wähle erneut Spielen.',
     'Le impostazioni della stanza si stanno ancora sincronizzando. Riprova Gioca.'),
    ('Cannot verify this device for Ranked. Check access to the OS device identifier.',
     'No se pudo verificar este dispositivo para la Clasificatoria. Revisa el acceso al identificador de '
     'dispositivo del sistema.', S,
     "Impossible de vérifier cet appareil pour le mode Classé. Vérifie l'accès à l'identifiant d'appareil "
     'du système.',
     'Não foi possível verificar este dispositivo para a Ranqueada. Verifique o acesso ao identificador do '
     'dispositivo do sistema.',
     'Dieses Gerät konnte für die Rangliste nicht überprüft werden. Prüfe den Zugriff auf die Gerätekennung '
     'des Systems.',
     "Impossibile verificare questo dispositivo per la Classificata. Controlla l'accesso all'identificativo "
     'del dispositivo del sistema.'),
    # scripts/online_content.py: a game with other mods or patches than character mods (RyuNET's
    # online_content.go sends this same text when it refuses a search)
    ('Public matchmaking and Ranked allow only character, sound and HUD mods. Your game has other mods or '
     'patches: play them in private rooms.',
     'Las partidas públicas y la Clasificatoria solo permiten mods de personajes, sonidos y HUD personalizados. '
     'Tu juego tiene otros mods o parches: úsalos en salas privadas.', S,
     "Les parties publiques et le mode Classé n'acceptent que les mods de personnages, de sons et de HUD. Ton "
     "jeu a d'autres mods ou patchs : utilise-les dans les salons privés.",
     'As partidas públicas e a Ranqueada só permitem mods de personagens, sons e HUD personalizados. Seu jogo '
     'tem outros mods ou patches: use-os em salas privadas.',
     'Öffentliche Spiele und die Rangliste erlauben nur Charakter-, Sound- und HUD-Mods. Dein Spiel hat andere '
     'Mods oder Patches: nutze sie in privaten Räumen.',
     'Le partite pubbliche e la Classificata consentono solo mod di personaggi, suoni e HUD personalizzati. Il '
     'tuo gioco ha altre mod o patch: usale nelle stanze private.'),
    # the server's ranked_account.go sends this same text when it refuses a search
    ('Ranked needs a Discord or Steam account linked to every player. Link yours in the launcher and try again.',
     'La Clasificatoria necesita una cuenta de Discord o Steam vinculada a cada jugador. Vincula la tuya en el '
     'launcher y vuelve a intentarlo.', S,
     'Le mode Classé nécessite un compte Discord ou Steam lié pour chaque joueur. Lie le tien dans le launcher '
     'et réessaie.',
     'A Ranqueada precisa de uma conta do Discord ou da Steam vinculada a cada jogador. Vincule a sua no '
     'launcher e tente novamente.',
     'Die Rangliste braucht für jeden Spieler ein verknüpftes Discord- oder Steam-Konto. Verknüpfe deins im '
     'Launcher und versuche es erneut.',
     'La Classificata richiede un account Discord o Steam collegato per ogni giocatore. Collega il tuo nel '
     'launcher e riprova.'),
    ('Your party has %d players. Ranked supports at most 4, regardless of device. Reduce the party or choose '
     'Public Matchmaking (up to 8).',
     'Tu grupo tiene %d jugadores. La Clasificatoria admite como máximo 4, sin importar el dispositivo. '
     'Reduce el grupo o elige Partida pública (hasta 8).', S,
     'Ton groupe compte %d joueurs. Le mode Classé en accepte 4 au maximum, quel que soit l\'appareil. '
     'Réduis le groupe ou choisis le matchmaking public (jusqu\'à 8).',
     'Seu grupo tem %d jogadores. A Ranqueada aceita no máximo 4, em qualquer dispositivo. Reduza o grupo '
     'ou escolha Buscar Partida pública (até 8).',
     'Deine Gruppe hat %d Spieler. Die Rangliste erlaubt höchstens 4, unabhängig vom Gerät. Verkleinere die '
     'Gruppe oder wähle öffentliches Matchmaking (bis zu 8).',
     'Il tuo gruppo ha %d giocatori. La Classificata ne ammette al massimo 4, su qualsiasi dispositivo. '
     'Riduci il gruppo o scegli il matchmaking pubblico (fino a 8).'),
    ('Ranked supports at most 4 party members. Ranked was cancelled because another player joined. Your party '
     'stays together; use Public Matchmaking for up to 8.',
     'La Clasificatoria admite como máximo 4 miembros por grupo. Se canceló porque entró otro jugador. Tu '
     'grupo sigue unido; usa Partida pública para hasta 8.', S,
     "Le mode Classé accepte 4 membres de groupe au maximum. Il a été annulé car un autre joueur a rejoint. "
     "Ton groupe reste uni ; utilise le matchmaking public jusqu'à 8.",
     'A Ranqueada aceita no máximo 4 membros por grupo. Foi cancelada porque outro jogador entrou. Seu grupo '
     'continua junto; use Buscar Partida pública para até 8.',
     'Die Rangliste erlaubt höchstens 4 Gruppenmitglieder. Sie wurde abgebrochen, weil ein weiterer Spieler '
     'beigetreten ist. Deine Gruppe bleibt zusammen; nutze öffentliches Matchmaking für bis zu 8.',
     "La Classificata ammette al massimo 4 membri per gruppo. È stata annullata perché è entrato un altro "
     "giocatore. Il tuo gruppo resta unito; usa il matchmaking pubblico fino a 8."),
    ('Cannot verify party compatibility. Please retry before starting this race.',
     'No se pudo verificar la compatibilidad del grupo. Inténtalo de nuevo antes de empezar esta carrera.', S,
     'Impossible de vérifier la compatibilité du groupe. Réessaie avant de lancer cette course.',
     'Não foi possível verificar a compatibilidade do grupo. Tente de novo antes de iniciar esta corrida.',
     'Die Gruppenkompatibilität konnte nicht überprüft werden. Versuche es erneut, bevor dieses Rennen startet.',
     'Impossibile verificare la compatibilità del gruppo. Riprova prima di iniziare questa gara.'),
    ('Cannot verify party compatibility. Please retry before matchmaking.',
     'No se pudo verificar la compatibilidad del grupo. Inténtalo de nuevo antes de buscar partida.', S,
     'Impossible de vérifier la compatibilité du groupe. Réessaie avant le matchmaking.',
     'Não foi possível verificar a compatibilidade do grupo. Tente de novo antes de buscar partida.',
     'Die Gruppenkompatibilität konnte nicht überprüft werden. Versuche es vor dem Matchmaking erneut.',
     'Impossibile verificare la compatibilità del gruppo. Riprova prima del matchmaking.'),
    ('Cannot verify Ranked server and party compatibility. Please retry.',
     'No se pudo verificar el servidor de la Clasificatoria ni la compatibilidad del grupo. Inténtalo de nuevo.',
     S,
     'Impossible de vérifier le serveur du mode Classé et la compatibilité du groupe. Réessaie.',
     'Não foi possível verificar o servidor da Ranqueada e a compatibilidade do grupo. Tente de novo.',
     'Der Ranglisten-Server und die Gruppenkompatibilität konnten nicht überprüft werden. Versuche es erneut.',
     'Impossibile verificare il server della Classificata e la compatibilità del gruppo. Riprova.'),
    ('Cannot prepare the compatibility check. Please try again.',
     'No se pudo preparar la verificación de compatibilidad. Inténtalo de nuevo.', S,
     'Impossible de préparer la vérification de compatibilité. Réessaie.',
     'Não foi possível preparar a verificação de compatibilidade. Tente de novo.',
     'Die Kompatibilitätsprüfung konnte nicht vorbereitet werden. Versuche es erneut.',
     'Impossibile preparare il controllo di compatibilità. Riprova.'),
    ('Ranked status check timed out. Please try again.',
     'Se agotó el tiempo al revisar la Clasificatoria. Inténtalo de nuevo.', S,
     'La vérification du mode Classé a expiré. Réessaie.',
     'A verificação da Ranqueada expirou. Tente de novo.',
     'Die Ranglisten-Prüfung hat zu lange gedauert. Versuche es erneut.',
     'Il controllo della Classificata è scaduto. Riprova.'),
    ('Player compatibility check timed out. Please try again.',
     'Se agotó el tiempo al verificar la compatibilidad de los jugadores. Inténtalo de nuevo.', S,
     'La vérification de compatibilité des joueurs a expiré. Réessaie.',
     'A verificação de compatibilidade dos jogadores expirou. Tente de novo.',
     'Die Kompatibilitätsprüfung der Spieler hat zu lange gedauert. Versuche es erneut.',
     'Il controllo di compatibilità dei giocatori è scaduto. Riprova.'),
    ('Checking Ranked compatibility. Please press Play again shortly.',
     'Revisando la compatibilidad para la Clasificatoria. Vuelve a pulsar Jugar en un momento.', S,
     'Vérification de la compatibilité du mode Classé. Appuie de nouveau sur Jouer dans un instant.',
     'Verificando a compatibilidade da Ranqueada. Aperte Jogar de novo em instantes.',
     'Ranglisten-Kompatibilität wird geprüft. Wähle gleich erneut Spielen.',
     'Verifica della compatibilità per la Classificata. Premi di nuovo Gioca tra poco.'),
    ('Matchmaking lobby confirmation failed. Please search again.',
     'No se pudo confirmar la sala de la búsqueda. Vuelve a buscar.', S,
     'La confirmation du salon de matchmaking a échoué. Relance la recherche.',
     'Não foi possível confirmar a sala da busca. Busque de novo.',
     'Die Matchmaking-Lobby konnte nicht bestätigt werden. Suche erneut.',
     'Conferma della lobby di matchmaking non riuscita. Cerca di nuovo.'),
    ('Cannot prepare matchmaking search.',
     'No se pudo preparar la búsqueda de partida.', S,
     'Impossible de préparer la recherche de partie.',
     'Não foi possível preparar a busca de partida.',
     'Die Spielersuche konnte nicht vorbereitet werden.',
     'Impossibile preparare la ricerca della partita.'),
)


def column_of(language_id):
    """The table's column for a PS4 language id (BB_LANGUAGE), None for English."""
    try:
        return _BY_ID.get(int(language_id))
    except (TypeError, ValueError):
        return None


def column_of_suffix(suffix):
    """The column for a game text table's language suffix (en, mx, fr...), None for English."""
    return _BY_SUFFIX.get(str(suffix).lower())


def _rows():
    for row in TEXTS:
        english, values = row[0], list(row[1:])
        if values[1] is None:
            values[1] = values[0]
        yield english, dict(zip(LANGUAGES, values))


TABLE = dict(_rows())


def text(english, column=None):
    """English text in a column ('mx', 'es'...); the game's language (BB_LANGUAGE) when None."""
    if column is None:
        column = column_of(os.environ.get('BB_LANGUAGE', '1'))
    values = TABLE.get(english)
    return values[column] if values and column in values else english


def current_column():
    """The game language's column for this launch ('en' when English or Arabic)."""
    return column_of(os.environ.get('BB_LANGUAGE', '1')) or 'en'


def c_table():
    """src/runtime_texts.inc, generated."""
    def literal(s):
        out = []
        for b in s.encode('utf-8'):
            c = chr(b)
            if c in '"\\':
                out.append('\\' + c)
            elif 32 <= b < 127:
                out.append(c)
            else:
                out.append('\\%03o' % b)
        return '"' + ''.join(out) + '"'
    lines = ['/* Generated by tools/gen_octr_texts.py from scripts/octr_texts.py: do not edit. */',
             '/* {English, ' + ', '.join(LANGUAGES) + '} */']
    for english, values in TABLE.items():
        lines.append('{' + ', '.join(literal(v) for v in (english, *(values[c] for c in LANGUAGES))) + '},')
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    from pathlib import Path
    target = Path(__file__).resolve().parent.parent / 'src' / 'runtime_texts.inc'
    if '--check' in sys.argv:
        ok = target.is_file() and target.read_text(encoding='utf-8') == c_table()
        print('octr_texts: the C table is ' + ('current' if ok else 'OUT OF DATE (run tools/gen_octr_texts.py)'))
        sys.exit(0 if ok else 1)
    print(c_table(), end='')
