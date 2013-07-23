/*
 * This file is released under the terms of the Artistic License.  Please see
 * the file LICENSE, included in this package, for details.
 *
 * Copyright (C) 2002 Mark Wong & Open Source Development Labs, Inc.
 *
 * 13 May 2003
 */

#include <stdio.h>
#include <string.h>

#include "db.h"
#include "logging.h"
#include "transaction_data.h"
#include "dbc.h"
#include "common.h"

#include <libpq-fe.h>

static char dbname[32] = "dbt2";
static char pghost[32] = "localhost";
static char pgport[32] = "5432";
static char pguser[32] = "dbt2";

struct pgsql_context_t
{
	struct db_context_t base;
	PGconn *conn;
	int inTransaction;
};

static int
pgsql_commit_transaction(struct db_context_t *_dbc)
{
	struct pgsql_context_t *dbc = (struct pgsql_context_t*) _dbc;
	PGresult *res;

	if(!dbc->inTransaction)
		return OK;

	res = PQexec(dbc->conn, "COMMIT");
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
		LOG_ERROR_MESSAGE("%s", PQerrorMessage(dbc->conn));
	}
	PQclear(res);

	dbc->inTransaction = 0;

	return OK;
}

/* Open a connection to the database. */
static int
pgsql_connect_to_db(struct db_context_t *_dbc)
{
		struct pgsql_context_t *dbc = (struct pgsql_context_t*) _dbc;
        char buf[1024];

        if(strcmp(pghost, "localhost") == 0 &&
           strcmp(pgport, "5432") == 0) {
			sprintf(buf, "dbname=%s user=%s", dbname, pguser);
        } else {
			sprintf(buf, "host=%s port=%s dbname=%s user=%s", pghost, pgport, dbname, pguser);
        }
        dbc->conn = PQconnectdb(buf);
        if (PQstatus(dbc->conn) != CONNECTION_OK) {
                LOG_ERROR_MESSAGE("Connection to database '%s' failed.",
                        dbname);
                LOG_ERROR_MESSAGE("%s", PQerrorMessage(dbc->conn));
                PQfinish(dbc->conn);
                return ERROR;
        }
        return OK;
}

/* Disconnect from the database and free the connection handle. */
static int
pgsql_disconnect_from_db(struct db_context_t *_dbc)
{
	struct pgsql_context_t *dbc = (struct pgsql_context_t*) _dbc;
	PQfinish(dbc->conn);
	return OK;
}

static struct db_context_t *
pgsql_db_init()
{
	struct db_context_t *context = malloc(sizeof(struct pgsql_context_t));
	memset(context, 0, sizeof(struct pgsql_context_t));
	return context;
}

static int
pgsql_rollback_transaction(struct db_context_t *_dbc)
{
	struct pgsql_context_t *dbc = (struct pgsql_context_t*) _dbc;
	PGresult *res;

	if(!dbc->inTransaction)
		return STATUS_ROLLBACK;
	
	res = PQexec(dbc->conn, "ROLLBACK");
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
		LOG_ERROR_MESSAGE("%s", PQerrorMessage(dbc->conn));
	}
	PQclear(res);
	dbc->inTransaction = 0;

	return STATUS_ROLLBACK;
}

static int
pgsql_sql_execute(struct db_context_t *_dbc, char * query, struct sql_result_t * sql_result,
				  char * query_name)
{
	struct pgsql_context_t *dbc = (struct pgsql_context_t*) _dbc;
	PGresult *res;

	if (!dbc->inTransaction)
	{
		/* Start a transaction block. */
		res = PQexec(dbc->conn, "BEGIN");
		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
			LOG_ERROR_MESSAGE("%s", PQerrorMessage(dbc->conn));
			PQclear(res);
			return ERROR;
		}

		PQclear(res);
		dbc->inTransaction = 1;
	}
	res = PQexec(dbc->conn, query);
	if (!res || (PQresultStatus(res) != PGRES_COMMAND_OK &&
				 PQresultStatus(res) != PGRES_TUPLES_OK)) {
		LOG_ERROR_MESSAGE("%s", PQerrorMessage(dbc->conn));
		PQclear(res);
		return ERROR;
	}

	if(sql_result == NULL)
		PQclear(res);
	else
	{
		sql_result->result_set = res;
		sql_result->current_row = -1;
		sql_result->num_rows = PQntuples(res);
	}

	return OK;
}

static int
pgsql_sql_fetchrow(struct db_context_t *_dbc, struct sql_result_t * sql_result)
{
	struct pgsql_context_t *dbc = (struct pgsql_context_t*) _dbc;
	PGresult *res = (PGresult *)sql_result->result_set;
	sql_result->current_row++;
	if(sql_result->current_row >= PQntuples(res))
		return 0;
	return 1;
}

static int
pgsql_sql_close_cursor(struct db_context_t *_dbc, struct sql_result_t * sql_result)
{
	struct pgsql_context_t *dbc = (struct pgsql_context_t*) _dbc;
	PGresult *res = (PGresult *)sql_result->result_set;
	PQclear(res);
}

static char *
pgsql_sql_getvalue(struct db_context_t *_dbc, struct sql_result_t * sql_result, int field)
{
	struct pgsql_context_t *dbc = (struct pgsql_context_t*) _dbc;
	PGresult *res = (PGresult *)sql_result->result_set;
	char *tmp = NULL;
	if (sql_result->current_row < 0 ||sql_result->current_row >= PQntuples(res) || field > PQnfields(res))
	{
#ifdef DEBUG_QUERY
		LOG_ERROR_MESSAGE("pgsql_sql_getvalue: POSSIBLE NULL VALUE or ERROR\n\nRow: %d, Field: %d",
						  sql_result->current_row, field);
#endif
	return tmp;
	}

	if ((tmp = calloc(sizeof(char), PQgetlength(res, sql_result->current_row, field) + 1)))
		strcpy(tmp, PQgetvalue(res, sql_result->current_row, field));
	else
		LOG_ERROR_MESSAGE("dbt2_sql_getvalue: CALLOC FAILED for value from field=%d\n", field);
	return tmp;
}
static struct option *
pgsql_dbc_get_options()
{
#define N_PGSQL_OPT  4
	struct option *dbc_options = malloc(sizeof(struct option) * (N_PGSQL_OPT + 1));

	dbc_options[0].name = "dbname";
	dbc_options[0].has_arg = required_argument;
	dbc_options[0].flag = NULL;
	dbc_options[0].val = 0;

	dbc_options[1].name = "host";
	dbc_options[1].has_arg = required_argument;
	dbc_options[1].flag = NULL;
	dbc_options[1].val = 0;

	dbc_options[2].name = "port";
	dbc_options[2].has_arg = required_argument;
	dbc_options[2].flag = NULL;
	dbc_options[2].val = 0;

	dbc_options[3].name = "user";
	dbc_options[3].has_arg = required_argument;
	dbc_options[3].flag = NULL;
	dbc_options[3].val = 0;

	dbc_options[N_PGSQL_OPT].name = 0;
	dbc_options[N_PGSQL_OPT].has_arg = 0;
	dbc_options[N_PGSQL_OPT].flag = 0;
	dbc_options[N_PGSQL_OPT].val = 0;
	return dbc_options;
}

static int
pgsql_dbc_set_option(const char *optname, const char *optvalue)
{
	if(strncmp(optname, "dbname", 32) == 0 && optvalue != NULL)
		strncpy(dbname, optvalue, 32);
	else if(strncmp(optname, "host", 32) == 0 && optvalue != NULL)
		strncpy(pghost, optvalue, 32);
	else if(strncmp(optname, "port", 32) == 0 && optvalue != NULL)
	{
		/* check port */
		int port = atoi(optvalue);
		if(port < 0 || port > 65535)
		{
			/* XXX: a better way to raise a message. */
			printf("invalid port number: %s\n", optvalue);
			return ERROR;
		}
		strncpy(pgport, optvalue, 32);
	}
	else if(strncmp(optname, "user", 32) == 0 && optvalue != NULL)
		strncpy(pguser, optvalue, 32);
	return OK;
}

struct dbc_sql_operation_t pgsql_sql_operation =
{
	pgsql_db_init,
	pgsql_connect_to_db,
	pgsql_disconnect_from_db,
	pgsql_commit_transaction,
	pgsql_rollback_transaction,
	pgsql_sql_execute,
	pgsql_sql_fetchrow,
	pgsql_sql_close_cursor,
	pgsql_sql_getvalue
};

int
pgsql_dbc_init()
{
	struct dbc_info_t *pgsql_info = make_dbc_info(
		"postgresql",
		"for postgresql: --dbname=<dbname> --host=<host> --port=<port> --user=<user>");
	pgsql_info->is_forupdate_supported = 0;

	pgsql_info->dbc_sql_operation = &pgsql_sql_operation;
	pgsql_info->dbc_get_options = pgsql_dbc_get_options;
	pgsql_info->dbc_set_option = pgsql_dbc_set_option;
	dbc_manager_add(pgsql_info);
}
