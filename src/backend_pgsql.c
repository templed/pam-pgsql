/*
 * PAM authentication module for PostgreSQL
 * 
 * Based in part on pam_unix.c of FreeBSD. See COPYRIGHT
 * for licensing details.
 *
 * David D.W. Downey ("pgpkeys") <david-downey@codecastle.com> et al. (see COPYRIGHT)
 * William Grzybowski <william@agencialivre.com.br>
 * Ernesto Hernández-Novich <emhn@uniregistry.link>
 * Luis E. Muñoz <lem@uniregistry.link>
 */

#define _XOPEN_SOURCE 500
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#define __USE_MISC
#include <string.h>
#undef __USE_MISC
#include <strings.h>
#include <syslog.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <libpq-fe.h>
#include <sys/types.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <crypt.h>
#include <gcrypt.h>
#include <b64/cdecode.h>

#include "backend_pgsql.h"
#include "pam_pgsql.h"
#include "config.h"

static char *
crypt_makesalt(pw_scheme scheme);

/** Salted-hash support **/

/* Supported salted-hash algorithms */

typedef struct {
    char *signature;
    int   algorithm;
    int   buffer_size;
} algorithm_descriptor;

static const algorithm_descriptor
algo_map[] = {
   { "SSHA-512", GCRY_MD_SHA512, 64 },
   { "SSHA512",  GCRY_MD_SHA512, 64 },
   { "SSHA-384", GCRY_MD_SHA384, 48 },
   { "SSHA384",  GCRY_MD_SHA384, 48 },
   { "SSHA-256", GCRY_MD_SHA256, 32 },
   { "SSHA256",  GCRY_MD_SHA256, 32 },
   { "SSHA-224", GCRY_MD_SHA224, 28 },
   { "SSHA224",  GCRY_MD_SHA224, 28 },
   { "SMD5",     GCRY_MD_MD5,    16 },
   { "SSHA",     GCRY_MD_SHA1,   20 }
};

static const int
algo_map_length = 10;

/* Return pointer to descriptor given algorithm name */
static const algorithm_descriptor *
get_algorithm(char *signature) {
    for (int i = 0; i < algo_map_length ; i++) {
        if (!strcmp(signature,algo_map[i].signature)) {
            return &algo_map[i];
        }
    }
	SYSLOG("Unsupported salted-hash algorithm: '%s'", signature);
    return NULL;
}

/* Base64-decode payload into byte buffer */

static char *
b64dec_payload(const char *encoded_payload, int *count) {
    base64_decodestate  s;
    unsigned int        payload_size;
    char               *payload;

    payload_size = strlen( encoded_payload );
    payload      = (char *) malloc( payload_size );

    /* Use libb64 to decode */
    base64_init_decodestate(&s);
    *count = base64_decode_block( encoded_payload,
                                  payload_size,
                                  payload,
                                  &s);

    return payload;
}

/* Return true if `guess` matches salted-hash password `salted`,
 * given a particular `salt_length` */

static int
match(const char *salted, const char *guess, int salt_length) {

    int retval = 0;
    /* Overall goal of this code
     * 1. Extract the hash type, and salted digest from the input
     * 2. Digest the entered password using the salt from the input and the specified hash algorithm
     * 3. Return the result of comparing the two digests
     * ---
     * Use a stupid state machine to parse the password entry
     * States correspond to flag value:
     * 0 - Starting state, have yet to encounter the opening curly bracket
     * 1 - Encountered opening curly bracket of the hash type.
     *     The next iteration of the loop will record the starting position of the hash type
     * 2 - Currently iterating through the hash type string.
     *
     * Will break from the loop as soon as the closing curly is encountered
     */
    int flag = 0; // 0 - opening bracket, record next position. 1 - in hash_id, increment length accumulator
    int hash_id_start = -1;
    int hash_id_len = 0;
    for(int i = 0; i < strlen(salted); i++){
        if(salted[i] == '}') {
            break;
        }
        if(flag == 1) {
            hash_id_start = i;
            flag = 2;
        }
        if(flag == 2) {
            hash_id_len++;
        }
        if(salted[i] == '{') {
            flag = 1;
        }
    }

    char * digest_only = malloc(strlen(salted) + 1 - 2 - hash_id_len);  // -2 for '{}' , +1 for \0
    for(int i = 0 ; i < strlen(salted) - 2 - hash_id_len; i++){
        digest_only[i] = salted[i + (hash_id_start + hash_id_len - 1) + 2];
    }
    digest_only[strlen(salted) - 2 - hash_id_len] = 0;  // I really feel there should be a +1 here...

    char * hash_id = malloc(hash_id_len + 1);
    for(int i = 0; i < hash_id_len; i++){
        hash_id[i] = salted[i + 1 + hash_id_start - 1];
    }
    hash_id[hash_id_len + 1] = 0;

    /* Payload Base64 decode */
    int   payload_length;
    char *payload = b64dec_payload( digest_only, &payload_length );
    /* Payload is HASH(passwd || salt) || salt */

    /* Split using salt length */
    char *salt = payload + payload_length - salt_length;
    /* (payload,payload_length) describe HASH(passwd || salt)
     * (salt,salt_length) describe salt */
    /* Catenate guess with salt */
    int  guess_length    = strlen( guess );
    int  catenate_length = guess_length + salt_length;
    char *catenate       = malloc( catenate_length );

    strcpy( catenate, guess );
    for (int i = 0; i < salt_length ; i++)
        *(catenate + guess_length + i) = salt[i];
    /* Hash catenation */
    const algorithm_descriptor *p = get_algorithm( hash_id );

    /* Compare only for supported algorithms */
    if (p != NULL) {
        if (!gcry_check_version(NEED_LIBGCRYPT_VERSION)) {
            SYSLOG("libgcrypt is too old (need %s, have %s)\n", NEED_LIBGCRYPT_VERSION, gcry_check_version(NULL));
            retval = 0;
        } else {
            char *digest = calloc(1, p->buffer_size);

            /* Disable secure memory.  */
            gcry_control(GCRYCTL_DISABLE_SECMEM, 0);

            /* Tell Libgcrypt that initialization has completed. */
            gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

            gcry_md_hash_buffer(p->algorithm,
                                digest,
                                catenate,
                                catenate_length);
            retval = !memcmp(digest, payload, p->buffer_size);
            free(digest);
        }
    }
    free(catenate);
    free(digest_only);
    free(hash_id);

    return retval;
}


/* very private: used only in get_module_options */
static char *
build_conninfo(modopt_t *options)
{
    char *str;

	 if(options == NULL)
		 return NULL;

	 str = (char *) malloc(sizeof(char)*512);
    memset(str, 0, 512);

    /* SAFE */
	 if(options->db) {
		strcat(str, "dbname=");
		strncat(str, options->db, strlen(options->db));
	 }

	if(options->host) {
		strcat(str, " host=");
		strncat(str, options->host, strlen(options->host));
	}
	if(options->port) {
		strcat(str, " port=");
		strncat(str, options->port, strlen(options->port));
	}    
	if(options->timeout) {
		strcat(str, " connect_timeout=");
		strncat(str, options->timeout, strlen(options->timeout));
	}
	if(options->user) {
		strcat(str, " user=");
		strncat(str, options->user, strlen(options->user));
	}
	if(options->passwd) {
		strcat(str, " password=");
		strncat(str, options->passwd, strlen(options->passwd));
	}
	if(options->sslmode) {
		strcat(str, " sslmode=");
		strncat(str, options->sslmode, strlen(options->sslmode));
	}

	return str;
}

/* private: open connection to PostgreSQL */
PGconn *
db_connect(modopt_t *options)
{
	PGconn *conn;
	if(options->connstr == NULL)
		options->connstr = build_conninfo(options);

	conn = PQconnectdb(options->connstr);
	if(PQstatus(conn) != CONNECTION_OK) {
		SYSLOG("PostgreSQL connection failed: '%s'", PQerrorMessage(conn));
		return NULL;
	}
	return conn;
}

/* private: expand query; partially stolen from mailutils */

static int
expand_query (char **command, const char** values, const char *query, const char *service, const char *user, const char *passwd, const char *rhost, const char *raddr)
{
	char *p, *q, *res;
	unsigned int len;
	unsigned int nparm=0;
  
	if (!query) {
		*command = NULL;
		return 0;
	}
	/* Compute resulting query length */
	for (len = 0, p = (char *) query; *p; ) {
		if (*p == '%') {
			if (p[1] == 'u' || p[1] == 'p' || p[1] == 's') {
				len += 4; /*we allow 128 tokens max*/
				p += 2;
				continue;
			} else if (p[1] == '%') {
				len++;
				p += 2;
				continue;
			}
		}
		len++;
		p++;  
	}
	res = malloc (len + 1);
	if (!res) {
		*command = NULL;
		return 0;
	}
	for (p = (char *) query, q = res; *p; ) {
		if (*p == '%') {
			switch (*++p) {
				case 'u': {
					sprintf(q, "$%i", ++nparm);
					values[nparm-1] = user;
					q += strlen (q);
					p++;
				}
				break;
				case 'p': {
					sprintf(q, "$%i", ++nparm);
					values[nparm-1] = passwd;
					q += strlen (q);
					p++;
				}
				break;
				case 's': {
					sprintf(q, "$%i", ++nparm);
					values[nparm-1] = service;
					q += strlen (q);
					p++;
				}
				break;
				case 'h': {
					sprintf(q, "$%i", ++nparm);
					values[nparm-1] = rhost;
					q += strlen (q);
					p++;
				}
				break;
				case 'i': {
					sprintf(q, "$%i", ++nparm);
					values[nparm-1] = raddr;
					q += strlen (q);
					p++;
					if (!raddr) {
						if (strchr(rhost, '.') != NULL) {
							*command = NULL;
							free (res);
							return 0;
						}
					}
				}
				break;
				case '%':
				default:
					*q++ = *p++;
				break;
			}
		} else	*q++ = *p++;
	 }
	 *q = 0;
	 
	 *command = res;
	 values[nparm] = NULL; 
	 return nparm;
}

/* private: execute query */
int
pg_execParam(PGconn *conn, PGresult **res, 
        const char *query, const char *service, const char *user, const char *passwd, const char *rhost)
{
	int nparm = 0;
	const char *values[128];
	char *command, *raddr;
	struct hostent *hentry;

	if (!conn) 
		return PAM_AUTHINFO_UNAVAIL;
	bzero(values, sizeof(*values));
	
	raddr = NULL;
	
	if(rhost != NULL && (hentry = gethostbyname(rhost)) != NULL) {
		/* Make IP string */
		raddr = malloc(INET_ADDRSTRLEN);
		inet_ntop(AF_INET, hentry->h_addr_list[0], raddr, INET_ADDRSTRLEN);
	}
	
	nparm = expand_query(&command, values, query, service, user, passwd, rhost, raddr);
	if (command == NULL) 
		return PAM_AUTH_ERR;
	
	*res = PQexecParams(conn, command, nparm, 0, values, 0, 0, 0);
	free (command);
	free (raddr);
    
	if(PQresultStatus(*res) != PGRES_COMMAND_OK && PQresultStatus(*res) != PGRES_TUPLES_OK) {
		SYSLOG("PostgreSQL query failed: '%s'", PQresultErrorMessage(*res));
		return PAM_AUTHINFO_UNAVAIL;
	}
	return PAM_SUCCESS;
}

/* private: convert an integer to a radix 64 character */
static int
i64c(int i)
{
	if (i <= 0)
		return ('.');
	if (i == 1)
		return ('/');
	if (i >= 2 && i < 12)
		return ('0' - 2 + i);
	if (i >= 12 && i < 38)
		return ('A' - 12 + i);
	if (i >= 38 && i < 63)
		return ('a' - 38 + i);
	return ('z');
}

/* authenticate user and passwd against database */
int
backend_authenticate(const char *service, const char *user, const char *passwd, const char *rhost, modopt_t *options)
{
	PGresult *res;
	PGconn *conn;
	int rc, row_count;
	char *tmp;

	if (!(conn = db_connect(options)))
		return PAM_AUTH_ERR;

	DBGLOG("query: %s", options->query_auth);
	rc = PAM_AUTH_ERR;
	if (pg_execParam(conn, &res, options->query_auth, service, user, passwd, rhost) == PAM_SUCCESS) {
		row_count = PQntuples(res);
		if (row_count == 0) {
			rc = PAM_USER_UNKNOWN;
		} else {
			for (int i = 0; i < row_count && rc != PAM_SUCCESS; i++) {
				if (!PQgetisnull(res, i, 0)) {
					char *stored_pw = PQgetvalue(res, i, 0);
					if (options->pw_type == PW_FUNCTION) {
						if (!strcmp(stored_pw, "t")) {
							rc = PAM_SUCCESS;
						}
					} else {
						tmp = password_encrypt(options, user, passwd, stored_pw);
                        if (tmp != NULL && !strcmp(stored_pw, tmp)) {
							rc = PAM_SUCCESS;
						}
						free (tmp);
					}
				}
			}
		}
		PQclear(res);
	}
	PQfinish(conn);
	return rc;
}

/* private: encrypt password using the preferred encryption scheme */
char *
password_encrypt(modopt_t *options, const char *user, const char *pass, const char *salt)
{
	char *s = NULL;
	switch(options->pw_type) {
		case PW_CRYPT:
		case PW_CRYPT_MD5:
		case PW_CRYPT_SHA512: {
			char *c = NULL;
			if (salt==NULL) {
				c = crypt(pass, crypt_makesalt(options->pw_type));
			} else {
				c = crypt(pass, salt);
			}
			if (c!=NULL) {
				s = strdup(c);
			}
		}
		break;
		case PW_MD5: {
			unsigned char hash[16] = { 0, }; /* 16 is the md5 block size */
			int i;
			s = (char *) malloc(33); /* 32 bytes + 1 byte for \0 */

			gcry_md_hash_buffer(GCRY_MD_MD5, hash, pass, strlen(pass));

			for(i = 0; i < sizeof(hash); i++)
				sprintf(&s[i * 2], "%.2x", hash[i]);
		}
		break;
		case PW_MD5_POSTGRES: {
			/* This is the md5 variant used by postgres shadow table.
			cleartext is password||user
			returned value is md5||md5hash(password||user)
			*/
			unsigned char hash[16] = { 0, }; /* 16 is the md5 block size */
			int i;
			s = (char *) malloc(36); /* 3 bytes for "md5" + 32 bytes for the hash + 1 byte for \0 */
			memcpy(s, "md5", 3);

			size_t unencoded_length;
			char *unencoded;

			unencoded_length = strlen(pass)+strlen(user);
			unencoded = malloc(unencoded_length+1);
			sprintf(unencoded, "%s%s", pass, user);

			gcry_md_hash_buffer(GCRY_MD_MD5, hash, unencoded, strlen(unencoded));
			for(i = 0; i < sizeof(hash); i++)
				sprintf(&s[(i * 2) + 3], "%.2x", hash[i]);

			free(unencoded);

		}
		break;
		case PW_SHA1: {
			unsigned char hash[20] = { 0, }; /* 20 is the sha1 block size */
			int i;
			s = (char *) malloc(41); /* 40 bytes + 1 byte for \0 */

			gcry_md_hash_buffer(GCRY_MD_SHA1, hash, pass, strlen(pass));

			for(i = 0; i < sizeof(hash); i++)
				sprintf(&s[i * 2], "%.2x", hash[i]);
		}
		break;
        case PW_SALTEDHASH: {
            if (match( salt, pass, options->salt_size )) {
               s = strdup(salt);
            }
        }
        break;
		case PW_CLEAR:
		case PW_FUNCTION:
		default:
			s = strdup(pass);
	}
	return s;
}

static char *
crypt_makesalt(pw_scheme scheme)
{
	static char result[12];
	int len,pos;
	struct timeval now;

	if(scheme==PW_CRYPT){
		len=2;
		pos=0;
	} else if(scheme==PW_CRYPT_SHA512) { /* PW_CRYPT_SHA512 */
		strcpy (result, "$6$");
		len = 11;
		pos = 3;
	} else { /* PW_CRYPT_MD5 */
		strcpy(result,"$1$");
		len=11;
		pos=3;
	}
	gettimeofday(&now,NULL);
	srandom(now.tv_sec*10000+now.tv_usec/100+clock());
	while(pos<len)result[pos++]=i64c(random()&63);
	result[len]=0;
	return result;
}

