/*       teleinfoserial_mysql.c										*/
/* Version pour PC et wrt54gl										*/
/* Lecture données Téléinfo et enregistre données sur base mysql vesta si ok sinon dans fichier csv.	*/
/* Connexion par le port série du Wrt54gl (Console désactivée dans inittab.)				*/
/* Vérification checksum données téléinfo et boucle de 3 essais si erreurs.				*/
/* Par domos78 at free point fr										*/

/*
   Paramètres à adapter:
   - Port série à modifier en conséquence avec SERIALPORT.
   - Nombre de valeurs à relever: NB_VALEURS + tableaux "etiquettes" et "poschecksum" à modifier selon abonnement (ici triphasé heures creuses).
   - Paramètres Mysql (Serveur, Base, table et login/password)
   - Autoriser le serveur MySql à accepter les connexions distantes pour le Wrt54gl.

   Compilation PC:
   - gcc -Wall teleinfoserial_mysql.c -o teleinfoserial_mysql -lmysqlclient

   Compilation wrt54gl:
   - avec le SDK (OpenWrt-SDK-Linux).

   Résultat pour les données importantes dans la base MySql du serveur distant:
   dan@vesta:~$ bin/listdatateleinfo.sh
   timestamp       date            heure           hchp    hchc    ptec    inst1   inst2   inst3   papp
   1222265525      24/09/2008      16:12:05        8209506 8026019 HP      1       0       1       460
   1222265464      24/09/2008      16:11:04        8209499 8026019 HP      1       0       1       460
   1222265405      24/09/2008      16:10:05        8209493 8026019 HP      1       0       1       390
   1222265344      24/09/2008      16:09:04        8209487 8026019 HP      1       0       1       390
   1222265284      24/09/2008      16:08:04        8209481 8026019 HP      1       0       1       390
   1222265225      24/09/2008      16:07:05        8209476 8026019 HP      1       0       1       390
   1222265164      24/09/2008      16:06:04        8209470 8026019 HP      1       0       1       390
   1222265105      24/09/2008      16:05:05        8209464 8026019 HP      1       0       1       390

   Résultat en mode DEBUG:
   root@wrt54gl:~# ./teleinfoserial_mysql
   ----- 2008-10-12 15:59:52 -----
   ADCO='70060936xxxx'
   OPTARIF='HC..'
   ISOUSC='20'
   HCHP='008444126'
   HCHC='008228815'
   PTEC='HP'
   IINST1='002'
   IINST2='000'
   IINST3='001'
   IMAX1='011'
   IMAX2='020'
   IMAX3='019'
   PMAX='07470'
   PAPP='00610'
   HHPHC='E'
   MOTDETAT='000000'
   PPOT='00'
   ADIR1=''
   ADIR2=''
   ADIR3=''
   */

//-----------------------------------------------------------------------------
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#include <termios.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <mysql/mysql.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#define TRAMELOG "/tmp/teleinfotrame."

// Active mode debug.
//#define DEBUG
//#define NO_SQL

#ifdef NO_SQL
const bool sql_enabled = false;
#else
const bool sql_enabled = true;
#endif

struct config {
	char *device;
	char *mysql_host;
	char *mysql_db;
	char *mysql_table;
	char *mysql_login;
	char *mysql_pwd;
	char *csv_backup;
};

// taille max d'une trame
#define MAX_FRAME_LEN 256

/// Constantes/Variables à changer suivant abonnement, Nombre de valeurs, voir tableau "etiquettes", 20 pour abonnement tri heures creuse.
#define NB_VALEURS 11
#define VAL_MAX_SIZE 17

//char etiquettes[NB_VALEURS][16] = {"ADCO", "OPTARIF", "ISOUSC", "HCHP", "HCHC", "PTEC", "IINST1", "IINST2", "IINST3", "IMAX1", "IMAX2", "IMAX3", "PMAX", "PAPP", "HHPHC", "MOTDETAT", "PPOT", "ADIR1", "ADIR2" ,"ADIR3"};

char etiquettes[NB_VALEURS][16] = {
	"ADCO",
	"OPTARIF",
	"ISOUSC",
	"HCHC",
	"HCHP",
	"PTEC",
	"IINST",
	"IMAX",
	"PAPP",
	"HHPHC",
	"MOTDETAT",
};
// TODO: c'est mieux si on ne les ecrit pas 2 fois
const char *fields = "`TIMESTAMP`, `ADCO`, `OPTARIF`, `ISOUSC`, `HCHC`, `HCHP`, `PTEC`, `IINST`, `IMAX`, `PAPP`, `HHPHC`, `MOTDETAT`";

// Fin Constantes/variables à changées suivant abonnement.

char valeurs[NB_VALEURS][VAL_MAX_SIZE + 1];

/*------------------------------------------------------------------------------*/
/* Init port rs232								*/
/*------------------------------------------------------------------------------*/
// Mode Non-Canonical Input Processing, Attend 1 caractère ou time-out(avec VMIN et VTIME).
int initserie(char *serial)
{
	struct termios termiosteleinfo;
	int device;

	// Ouverture de la liaison serie (Nouvelle version de config.)
	device = open(serial, O_RDWR | O_NOCTTY);
	if (device == -1) {
		syslog(LOG_ERR, "Erreur ouverture du port serie %s !", serial);
		return -1;
	}

	tcgetattr(device, &termiosteleinfo);

	cfsetispeed(&termiosteleinfo, B1200);			// Configure le débit en entrée/sortie.
	cfsetospeed(&termiosteleinfo, B1200);

	termiosteleinfo.c_cflag |= (CLOCAL | CREAD);			// Active réception et mode local.

	// Format série "7E1"
	termiosteleinfo.c_cflag |= PARENB;				// Active 7 bits de donnees avec parite pair.
	termiosteleinfo.c_cflag &= ~PARODD;
	termiosteleinfo.c_cflag &= ~CSTOPB;
	termiosteleinfo.c_cflag &= ~CSIZE;
	termiosteleinfo.c_cflag |= CS7;

	termiosteleinfo.c_iflag |= (INPCK | ISTRIP);			// Mode de control de parité.

	termiosteleinfo.c_cflag &= ~CRTSCTS;				// Désactive control de flux matériel.

	termiosteleinfo.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);	// Mode non-canonique (mode raw) sans echo.

	termiosteleinfo.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);	// Désactive control de flux logiciel, conversion 0xOD en 0x0A.

	termiosteleinfo.c_oflag &= ~OPOST;				// Pas de mode de sortie particulier (mode raw).

	termiosteleinfo.c_cc[VTIME] = 10;				// time-out en decisecondes
	termiosteleinfo.c_cc[VMIN]  = 0;				// 1 car. attendu.

	tcflush(device, TCIFLUSH);					// Efface les données reçues mais non lues.
	tcsetattr(device, TCSANOW, &termiosteleinfo);			// Sauvegarde des nouveaux parametres

	return device;
}

/*------------------------------------------------------------------------------*/
/* Lecture données téléinfo sur port série					*/
/*------------------------------------------------------------------------------*/
int LiTrameSerie(int device, char *message, size_t max_len)
{
	int res = 0;
	int err = 0;
	size_t nb = 0;
	char *ptr = message;
	char c;

	// Efface les données non lus en entrée.
	tcflush(device, TCIFLUSH);
	*ptr ='\0';

	// recherche du debut de la trame (0x02)
	do {
		res = read(device, &c, 1);
		if (res < 0) {
			syslog(LOG_ERR, "Erreur de lecture de données Téléinfo : %s",
			       strerror(errno));
			err = 1;
		}
		if (res == 0) {
			syslog(LOG_ERR, "Rien à lire sur la Téléinfo");
			err = 1;
		}
		nb++;
		// conditions d'arret:
		// - erreur / timeout
		// - STX trouvé
		// - STX non reçu sur 2 trames
	} while ( (c != 0x02) && (err == 0) && (nb < (max_len * 2)));

	if (err || (c != 0x02)) {
		err = 1;
		goto out;
	}

	// lecture jusqu'à la trame suivante
	do {
		res = read(device, ptr++, 1);
		if (res < 0) {
			syslog(LOG_ERR, "Erreur de lecture de données Téléinfo : %s",
			       strerror(errno));
			err = 1;
		}
		if (res == 0) {
			syslog(LOG_ERR, "Rien à lire sur la Téléinfo");
			err = 1;
		}
		// conditions d'arret:
		// - erreur / timeout
		// - STX trouvé
		// - buffer plein
	} while ((*(ptr - 1) != 0x02 ) && (err == 0) && (ptr - message < (int)max_len));

	*ptr = '\0';
out:
	return err;
}

/*------------------------------------------------------------------------------*/
/* Test checksum d'un message (Return 1 si checkum ok)				*/
/*------------------------------------------------------------------------------*/
int checksum_ok(char *etiquette, char *valeur, char checksum)
{
	unsigned char sum = 0x20; /* espace entre etiquette et valeur */
	unsigned int i;

	for (i = 0; i < strlen(etiquette); i++)
		sum = sum + etiquette[i];

	for (i=0; i < strlen(valeur); i++)
		sum = sum + valeur[i];

	sum = (sum & 0x3F) + 0x20; /* c'est bizarre, mais c'est comme ça */

	if (sum == checksum)
		return 0;
#ifdef DEBUG
	syslog(LOG_INFO, "Checksum lu:%02x   calculé:%02x", checksum, sum);
#endif
	return 1;
}

/*------------------------------------------------------------------------------*/
/* Recherche valeurs des étiquettes de la liste.				*/
/*------------------------------------------------------------------------------*/
int LitValEtiquettes(const char *message)
{
	char *checksum = NULL;
	char *val = NULL;
	char *match;
	int id;
	int res;
	int err = 1;

	for (id = 0; id < NB_VALEURS; id++) {
		match = strstr(message, etiquettes[id]);
		if (match) {
			res = sscanf(match, "%*s %as %as", &val, &checksum);
			if (res < 2) {
				syslog(LOG_ERR, "Donnees teleinfo corrompues");
				goto out;
			}

			// le checksum peut être un espace, et dans ce cas,
			// scanf matche l'etiquette suivante.
			// Si c'est le cas, on le force à espace.
			if (strlen(checksum) > 1)
				checksum[0]=' ';

			if (strlen(val) > VAL_MAX_SIZE) {
				syslog(LOG_ERR, "Trop grande valeur %zu pour %s",
				       strlen(val), etiquettes[id]);
				goto out;
			}
			strcpy(valeurs[id], val);
			free(val);
			val = NULL;
			err = checksum_ok(etiquettes[id], valeurs[id], checksum[0]);
			if (err) {
				syslog(LOG_ERR, "Donnees teleinfo [%s] corrompues !",
				       etiquettes[id]);
				goto out;
			}
		}
	}
	// TODO: ?! WTF ?
	// Remplace chaine "HP.." ou "HC.." par "HP ou "HC".
	//valeurs[5][2] = '\0';
out:
#ifdef DEBUG
	printf("----------------------\n");
	for (id=0; id<NB_VALEURS; id++)
		printf("%s='%s'\n", etiquettes[id], valeurs[id]);
#endif
	free(checksum);
	free(val);
	return err;
}

/*------------------------------------------------------------------------------*/
/* Ecrit les données teleinfo dans base mysql					*/
/*------------------------------------------------------------------------------*/
int writemysqlteleinfo(MYSQL *mysql, const char *table, const char *data)
{
	char *query = NULL;
	int err = 1;

	if(!mysql) {
		syslog(LOG_ERR, "Erreur: MySQL non initialisée !");
		goto out;
	}
	asprintf(&query, "INSERT INTO `%s` (%s) VALUES (%s)", table, fields, data);
	if(mysql_query(mysql, query)) {
		syslog(LOG_ERR, "Erreur INSERT %d: \%s", mysql_errno(mysql), mysql_error(mysql));
		goto out;
	}
#ifdef DEBUG
	else syslog(LOG_INFO, "Requete MySql ok.");
#endif
	err = 0;
out:
	free(query);
	return err;
}

/*------------------------------------------------------------------------------*/
/* Ecrit les données teleinfo dans fichier DATACSV				*/
/*------------------------------------------------------------------------------*/
int writecsvteleinfo(const char *file, const char *data)
{
	/* Ouverture fichier csv */
	FILE *datateleinfo;

	if ((datateleinfo = fopen(file, "a")) == NULL) {
		syslog(LOG_ERR, "Erreur ouverture fichier teleinfo %s !", file);
		return 1;
	}
	fprintf(datateleinfo, "%s\n", data);
	fclose(datateleinfo);
	return 0;
}

#ifdef DEBUG
/*------------------------------------------------------------------------------*/
/* Ecrit la trame teleinfo dans fichier si erreur (pour debugger)		*/
/*------------------------------------------------------------------------------*/
void writetrameteleinfo(const char *trame, const char *ts)
{
	char nomfichier[] = TRAMELOG;
	strcat(nomfichier, ts);
	FILE *teleinfotrame;

	if ((teleinfotrame = fopen(nomfichier, "w")) == NULL)
	{
		syslog(LOG_ERR, "Erreur ouverture fichier teleinfotrame %s !", nomfichier);
		exit(1);
	}
	fprintf(teleinfotrame, "%s", trame);
	fclose(teleinfotrame);
}
#endif

int parse_config(char *file, struct config *c)
{
	FILE *f;
	char *key = NULL;
	char *value = NULL;
	int err = 1;

	f = fopen(file, "r");
	if (!f) {
		fprintf(stderr, "Erreur: impossible d'ouvrir %s\n", file);
		goto out;
	}
	while (fscanf(f, "%as = %as", &key, &value) == 2) {
		if (!strcmp(key, "device")) c->device = strdup(value);
		if (!strcmp(key, "mysql_host")) c->mysql_host = strdup(value);
		if (!strcmp(key, "mysql_db")) c->mysql_db = strdup(value);
		if (!strcmp(key, "mysql_table")) c->mysql_table = strdup(value);
		if (!strcmp(key, "mysql_login")) c->mysql_login = strdup(value);
		if (!strcmp(key, "mysql_pwd")) c->mysql_pwd = strdup(value);
		if (!strcmp(key, "csv_backup")) c->csv_backup = strdup(value);
		free(key);
		free(value);
		key = value = NULL;
	}

	// csv_backup peut être null
	if (c->device && c->mysql_host && c->mysql_db && c->mysql_table &&
	    c->mysql_login && c->mysql_pwd)
		err = 0;
out:
	free(key);
	free(value);
	return err;
}

void free_config(struct config *c)
{
	free(c->device);
	free(c->mysql_host);
	free(c->mysql_db);
	free(c->mysql_table);
	free(c->mysql_login);
	free(c->mysql_pwd);
	free(c->csv_backup);
	memset(c, 0, sizeof(*c));
}

void usage(char *prog)
{
	printf("Usage: %s config_file\n", prog);
}

/*------------------------------------------------------------------------------*/
/* Main										*/
/*------------------------------------------------------------------------------*/
int main(int argc, char **argv)
{
	char message[MAX_FRAME_LEN];
	char datateleinfo[512];
	int erreur_checksum;
	int fdserial;
	MYSQL mysql;
	my_bool auto_reconnect = 1;
	struct tm *dc;
	struct config conf;
	char timestamp[11];
	int no_essais = 1;
	int nb_essais = 3;

	time_t td;
	int err = 1;
	int i;

	memset(&conf, 0, sizeof(conf));

	if (argc != 2) {
		usage(argv[0]);
		goto out;
	}

	if (access(argv[1], R_OK)) {
		fprintf(stderr, "Erreur: fichier de configuration inacessible: %s\n",
			strerror(errno));
		usage(argv[0]);
		goto out;
	}

	err = parse_config(argv[1], &conf);
	if (err) {
		fprintf(stderr, "Erreur: fichier de configuration incomplet\n");
		goto err_config;
	}

	openlog("teleinfoserial_mysql", LOG_PID, LOG_USER);

	fdserial = initserie(conf.device);
	if (fdserial < 0) {
		syslog(LOG_ERR, "Erreur: Initialisation MySQL impossible !");
		goto err_init_serial;
	}

	if (sql_enabled) {
		if(!mysql_init(&mysql)) {
			syslog(LOG_ERR, "Erreur: Initialisation MySQL impossible !");
			goto err_init_bd;
		}

		mysql_options(&mysql, MYSQL_OPT_RECONNECT, &auto_reconnect);

		if(!mysql_real_connect(&mysql, conf.mysql_host,
				       conf.mysql_login, conf.mysql_pwd,
				       conf.mysql_db, 0, NULL, 0)) {
			syslog(LOG_ERR, "Erreur connection %d: %s",
			       mysql_errno(&mysql), mysql_error(&mysql));
			goto err_init_bd;
		}
	}

	while (true) {
		// Lit trame téléinfo.
		memset(valeurs, 0x00, sizeof(valeurs));
		err = LiTrameSerie(fdserial, message, MAX_FRAME_LEN);
		if (err) {
			syslog(LOG_ERR, "Erreur de lecture d'une trame. Arrêt.");
			goto err_frame;
		}

		td = time(NULL);
		dc = localtime(&td);
		strftime(timestamp, sizeof timestamp, "%s", dc);

#ifdef DEBUG
		writetrameteleinfo(message, timestamp);	// Enregistre trame en mode debug.
#endif

		// Lit valeurs des étiquettes de la liste.
		erreur_checksum = LitValEtiquettes(message);
		if (!erreur_checksum) {
			sprintf(datateleinfo, "'%ld',", td);
			for (i = 0; i < NB_VALEURS; i++) {
				strcat(datateleinfo, "'");
				strcat(datateleinfo, valeurs[i]);
				strcat(datateleinfo, "',");
			}
			// suppression de la derniere virgule
			datateleinfo[strlen(datateleinfo) - 1] = '\0';

			// Si écriture dans base MySql KO, écriture dans fichier csv.
			if (sql_enabled) {
				err = writemysqlteleinfo(&mysql, conf.mysql_table, datateleinfo);
			}

			if (err)
				(void) writecsvteleinfo(conf.csv_backup, datateleinfo);
		} else {
#ifdef DEBUG
			// Si erreur checksum enregistre trame.
			writetrameteleinfo(message, timestamp);
#endif
		}
		no_essais++;
		if ((erreur_checksum) && (no_essais <= nb_essais))
			break;
		sleep(60);
	}

	if (!erreur_checksum && !err)
		err = 0;

err_frame:
	if (sql_enabled)
		mysql_close(&mysql);
err_init_bd:
	close(fdserial);
err_init_serial:
	closelog();
err_config:
	free_config(&conf);
out:
	return err;
}
