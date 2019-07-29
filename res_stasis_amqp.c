/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright 2013-2017 The Wazo Authors  (see the AUTHORS file)
 *
 * David M. Lee, II <dlee@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*!
 * \brief Statsd channel stats. Exmaple of how to subscribe to Stasis events.
 *
 * This module subscribes to the channel caching topic and issues statsd stats
 * based on the received messages.
 *
 * \author David M. Lee, II <dlee@digium.com>
 * \since 12
 */

/*** MODULEINFO
	<depend>res_stasis_amqp</depend>
	<defaultenabled>no</defaultenabled>
	<support_level>extended</support_level>
 ***/

/*** DOCUMENTATION
	<configInfo name="res_stasis_amqp" language="en_US">
		<synopsis>Stasis to AMQP Backend</synopsis>
		<configFile name="stasis_amqp.conf">
			<configObject name="global">
				<synopsis>Global configuration settings</synopsis>
				<configOption name="loguniqueid">
					<synopsis>Determines whether to log the uniqueid for calls</synopsis>
					<description>
						<para>Default is no.</para>
					</description>
				</configOption>
				<configOption name="connection">
					<synopsis>Name of the connection from amqp.conf to use</synopsis>
					<description>
						<para>Specifies the name of the connection from amqp.conf to use</para>
					</description>
				</configOption>
				<configOption name="queue">
					<synopsis>Name of the queue to post to</synopsis>
					<description>
						<para>Defaults to asterisk_stasis</para>
					</description>
				</configOption>
				<configOption name="exchange">
					<synopsis>Name of the exchange to post to</synopsis>
					<description>
						<para>Defaults to empty string</para>
					</description>
				</configOption>
			</configObject>
		</configFile>
	</configInfo>
 ***/


#include "asterisk.h"

#include "asterisk/module.h"
#include "asterisk/stasis.h"
#include "asterisk/stasis_app.h"
#include "asterisk/stasis_message_router.h"
#include "asterisk/stasis_bridges.h"
#include "asterisk/ari.h"
#include "asterisk/time.h"
#include "asterisk/config_options.h"
#include "asterisk/manager.h"
#include "asterisk/json.h"
#include "asterisk/utils.h"

#include "asterisk/stasis_amqp.h"
#include "asterisk/amqp.h"

#define CONF_FILENAME "stasis_amqp.conf"
#define ROUTING_KEY_LEN 256

/*!
 * The ast_sched_context used for stasis application polling
 */
static struct ast_sched_context *stasis_app_sched_context;

/*! Regular Stasis subscription */
static struct stasis_subscription *sub;
static struct stasis_subscription *manager;

static int app_cmp(void *obj, void *arg, int flags);
static void destroy_string(void *obj);
static int setup_amqp(void);
static int publish_to_amqp(const char *topic, const char *name, const struct ast_eid *eid, struct ast_json *body);
char *new_routing_key(const char *prefix, const char *suffix);
struct ast_eid *eid_copy(const struct ast_eid *eid);

/*! \brief stasis_amqp configuration */
struct stasis_amqp_conf {
	struct stasis_amqp_global_conf *global;
};

/*! \brief global config structure */
struct stasis_amqp_global_conf {
	AST_DECLARE_STRING_FIELDS(
		/*! \brief connection name */
		AST_STRING_FIELD(connection);
		/*! \brief queue name */
		AST_STRING_FIELD(queue);
		/*! \brief exchange name */
		AST_STRING_FIELD(exchange);
	);
	/*! \brief current connection to amqp */
	struct ast_amqp_connection *amqp;
};

/*! \brief Locking container for safe configuration access. */
static AO2_GLOBAL_OBJ_STATIC(confs);

static struct aco_type global_option = {
	.type = ACO_GLOBAL,
	.name = "global",
	.item_offset = offsetof(struct stasis_amqp_conf, global),
	.category = "^global$",
	.category_match = ACO_WHITELIST,
};

static struct aco_type *global_options[] = ACO_TYPES(&global_option);

static void conf_global_dtor(void *obj)
{
	struct stasis_amqp_global_conf *global = obj;
	ao2_cleanup(global->amqp);
	ast_string_field_free_memory(global);
}

static struct stasis_amqp_global_conf *conf_global_create(void)
{
	RAII_VAR(struct stasis_amqp_global_conf *, global, NULL, ao2_cleanup);
	global = ao2_alloc(sizeof(*global), conf_global_dtor);
	if (!global) {
		return NULL;
	}
	if (ast_string_field_init(global, 64) != 0) {
		return NULL;
	}
	aco_set_defaults(&global_option, "global", global);
	return ao2_bump(global);
}


/*! \brief The conf file that's processed for the module. */
static struct aco_file conf_file = {
	/*! The config file name. */
	.filename = CONF_FILENAME,
	/*! The mapping object types to be processed. */
	.types = ACO_TYPES(&global_option),
};

static void conf_dtor(void *obj)
{
	struct stasis_amqp_conf *conf = obj;
	ao2_cleanup(conf->global);
}


static void *conf_alloc(void)
{
	RAII_VAR(struct stasis_amqp_conf *, conf, NULL, ao2_cleanup);
	conf = ao2_alloc_options(sizeof(*conf), conf_dtor,
		AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!conf) {
		return NULL;
	}
	conf->global = conf_global_create();
	if (!conf->global) {
		return NULL;
	}
	return ao2_bump(conf);
}

CONFIG_INFO_STANDARD(cfg_info, confs, conf_alloc,
	.files = ACO_FILES(&conf_file),
	.pre_apply_config = setup_amqp,
);

static int setup_amqp(void)
{
	struct stasis_amqp_conf *conf = aco_pending_config(&cfg_info);
	if (!conf) {
		return 0;
	}
	if (!conf->global) {
		ast_log(LOG_ERROR, "Invalid stasis_amqp.conf\n");
		return -1;
	}
	/* Refresh the AMQP connection */
	ao2_cleanup(conf->global->amqp);
	conf->global->amqp = ast_amqp_get_connection(conf->global->connection);
	if (!conf->global->amqp) {
		ast_log(LOG_ERROR, "Could not get AMQP connection %s\n",
			conf->global->connection);
		return -1;
	}
	return 0;
}

static int manager_event_to_json(struct ast_json *json, const char *event_name, char *fields)
{
	struct ast_json *json_value = NULL;
	char *line = NULL;
	char *word = NULL;
	char *key, *value;
	int res = 0;

	json_value = ast_json_string_create(event_name);
	if (!json_value) {
		return -1;
	}

	res = ast_json_object_set(json, "Event", json_value);
	if (res) {
		return -1;
	}

	while ((line = strsep(&fields, "\r\n")) != NULL) {
		key = NULL;
		value = NULL;

		while ((word = strsep(&line, ": ")) != NULL) {
			if (!key) {
				key = word;
			} else {
				value = word;
			}
		}

		json_value = ast_json_string_create(value);
		if (!json_value) {
			continue;
		}

		res = ast_json_object_set(json, key, json_value);
		if (res) {
			ast_log(LOG_DEBUG, "failed to set json value %s: %s\n", key, value);
			return -1;
		}
	}

	return 0;
}

/*!
 * \brief Subscription callback for all AMI messages.
 * \param data Data pointer given when creating the subscription.
 * \param sub This subscription.
 * \param topic The topic the message was posted to. This is not necessarily the
 *              topic you subscribed to, since messages may be forwarded between
 *              topics.
 * \param message The message itself.
 */
static void send_ami_event_to_amqp(void *data, struct stasis_subscription *sub,
									struct stasis_message *message)
{
	RAII_VAR(struct ast_json *, json, NULL, ast_json_unref);
	RAII_VAR(char *, routing_key, NULL, ast_free);
	RAII_VAR(struct ast_manager_event_blob *, manager_blob, NULL, ao2_cleanup);
	const char *routing_key_prefix = "stasis.ami";
	int res = 0;

	manager_blob = stasis_message_to_ami(message);
	json = ast_json_object_create();

	if (!manager_blob) {
		return;
	}

	if (!json) {
		return;
	}

	RAII_VAR(char *, fields, NULL, ast_free);
	fields = ast_strdup(manager_blob->extra_fields);

	res = manager_event_to_json(json, manager_blob->manager_event, fields);
	if (res) {
		ast_log(LOG_ERROR, "failed to create AMI message json payload for %s\n", manager_blob->extra_fields);
		return;
	}

	if (!(routing_key = new_routing_key(routing_key_prefix, manager_blob->manager_event))) {
		return;
	}

	publish_to_amqp(routing_key, manager_blob->manager_event, stasis_message_eid(message), json);
}

char *new_routing_key(const char *prefix, const char *suffix)
{
	char *ptr = NULL;
	char *routing_key = NULL;
	RAII_VAR(char *, lowered_suffix, NULL, ast_free);
	size_t routing_key_len = strlen(prefix) + strlen(suffix) + 1; /* "prefix.suffix" */

	if (!(lowered_suffix = ast_strdup(suffix))) {
		ast_log(LOG_ERROR, "failed to copy a routing key suffix\n");
		return NULL;
	}

	for (ptr = lowered_suffix; *ptr != '\0'; ptr++) {
		*ptr = tolower(*ptr);
	}

	if (!(routing_key = ast_malloc(routing_key_len + 1))) {
		ast_log(LOG_ERROR, "failed to allocate a string for the routing key\n");
		return NULL;
	}

	if (!(snprintf(routing_key, routing_key_len + 1, "%s.%s", prefix, lowered_suffix))) {
		ast_log(LOG_ERROR, "failed to format the routing key\n");
		return NULL;
	}
	return routing_key;
}

struct ast_eid *eid_copy(const struct ast_eid *eid)
{
	struct ast_eid *new = NULL;
	int i = 0;

	if (!(new = ast_calloc(sizeof(*new), 1))) {
		return NULL;
	}

	for (i = 0; i < 6; i++) {
		new->eid[i] = eid->eid[i];
	}
	return new;
}

static int publish_to_amqp(const char *topic, const char *name, const struct ast_eid *eid, struct ast_json *body)
{
	RAII_VAR(struct stasis_amqp_conf *, conf, NULL, ao2_cleanup);
	RAII_VAR(char *, msg, NULL, ast_json_free);
	RAII_VAR(struct ast_json *, json_msg, NULL, ast_json_free);
	RAII_VAR(struct ast_json *, json_name, NULL, ast_json_unref);
	RAII_VAR(struct ast_json *, json_eid, NULL, ast_json_unref);
	RAII_VAR(struct ast_eid *, message_eid, NULL, ast_free);
	char eid_str[128];
	int res;

	message_eid = eid_copy(eid != NULL ? eid : &ast_eid_default);
	ast_eid_to_str(eid_str, sizeof(eid_str), message_eid);
	if ((json_eid = ast_json_string_create(eid_str)) == NULL) {
		ast_log(LOG_ERROR, "failed to create json string\n");
		return -1;
	}

	if ((json_name = ast_json_string_create(name)) == NULL) {
		ast_log(LOG_ERROR, "failed to create json string\n");
		return -1;
	}

	if ((json_msg = ast_json_object_create()) == NULL) {
		ast_log(LOG_ERROR, "failed to create json object\n");
		return -1;
	}

	if (ast_json_object_set(json_msg, "event", json_name)) {
		ast_log(LOG_ERROR, "failed to set event name\n");
		return -1;
	}

	if (ast_json_object_set(json_msg, "eid", json_eid)) {
		ast_log(LOG_ERROR, "failed to set event eid\n");
		return -1;
	}

	if (ast_json_object_set(json_msg, "data", body)) {
		ast_log(LOG_ERROR, "failed to set event data\n");
		return -1;
	}

	if ((msg = ast_json_dump_string(json_msg)) == NULL) {
		ast_log(LOG_ERROR, "failed to convert json to string\n");
		return -1;
	}

	amqp_basic_properties_t props = {
		._flags = AMQP_BASIC_DELIVERY_MODE_FLAG | AMQP_BASIC_CONTENT_TYPE_FLAG,
		.delivery_mode = 2, /* persistent delivery mode */
		.content_type = amqp_cstring_bytes("application/json")
	};

	conf = ao2_global_obj_ref(confs);

	ast_assert(conf && conf->global && conf->global->amqp);

	res = ast_amqp_basic_publish(conf->global->amqp,
		amqp_cstring_bytes(conf->global->exchange),
		amqp_cstring_bytes(topic),
		0, /* mandatory; don't return unsendable messages */
		0, /* immediate; allow messages to be queued */
		&props,
		amqp_cstring_bytes(msg));

	if (res != 0) {
		ast_log(LOG_ERROR, "Error publishing stasis to AMQP\n");
		return -1;
	}

	return 0;
}


static int load_config(int reload)
{
	RAII_VAR(struct stasis_amqp_conf *, conf, NULL, ao2_cleanup);
	RAII_VAR(struct ast_amqp_connection *, amqp, NULL, ao2_cleanup);

	if (aco_info_init(&cfg_info) != 0) {
		ast_log(LOG_ERROR, "Failed to initialize config\n");
		aco_info_destroy(&cfg_info);
		return -1;
	}

	aco_option_register(&cfg_info, "connection", ACO_EXACT,
		global_options, "", OPT_STRINGFIELD_T, 0,
		STRFLDSET(struct stasis_amqp_global_conf, connection));
	aco_option_register(&cfg_info, "queue", ACO_EXACT,
		global_options, "asterisk_stasis", OPT_STRINGFIELD_T, 0,
		STRFLDSET(struct stasis_amqp_global_conf, queue));
	aco_option_register(&cfg_info, "exchange", ACO_EXACT,
		global_options, "", OPT_STRINGFIELD_T, 0,
		STRFLDSET(struct stasis_amqp_global_conf, exchange));


	switch (aco_process_config(&cfg_info, reload)) {
	case ACO_PROCESS_ERROR:
		return -1;
	case ACO_PROCESS_OK:
	case ACO_PROCESS_UNCHANGED:
		break;
	}
	conf = ao2_global_obj_ref(confs);
	if (!conf || !conf->global) {
		ast_log(LOG_ERROR, "Error obtaining config from stasis_amqp.conf\n");
		return -1;
	}
	return 0;
}

static int unload_module(void)
{
	if (stasis_app_sched_context) {
		ast_sched_context_destroy(stasis_app_sched_context);
		stasis_app_sched_context = NULL;
	}

	stasis_unsubscribe_and_join(sub);
	stasis_unsubscribe_and_join(manager);
	sub = NULL;
	manager = NULL;
	return 0;
}

static void stasis_amqp_message_handler(void *data, const char *app_name, struct ast_json *message)
{
	ast_debug(4, "called stasis amqp handler for application: '%s'\n", app_name);
	RAII_VAR(char *, routing_key, NULL, ast_free);
	const char *routing_key_prefix = "stasis.app";

	if (!(routing_key = new_routing_key(routing_key_prefix, app_name))) {
		return;
	}

	ast_debug(3, "publishing with routing key: '%s'\n", routing_key);
	publish_to_amqp(routing_key, "stasis_app", NULL, message);

	return;
}

int ast_subscribe_to_stasis(const char *app_name)
{
	int res = 0;
	ast_debug(1, "called subscribe to stasis for application: '%s'\n", app_name);
	res = stasis_app_register_all(app_name, &stasis_amqp_message_handler, NULL);
	return res;
}

int ast_unsubscribe_from_stasis(const char *app_name)
{
	ast_debug(3, "called unsubscribe from stasis\n");
	struct stasis_app *app = stasis_app_get_by_name(app_name);

	if (!app) {
		return -1;
	}

	stasis_app_unregister(app_name);

	return 0;
}

static int load_module(void)
{
	if (load_config(0) != 0) {
		ast_log(LOG_WARNING, "Configuration failed to load\n");
		return AST_MODULE_LOAD_DECLINE;
	}

	/* Subscription to receive all of the messages from manager topic */
	manager = stasis_subscribe(ast_manager_get_topic(), send_ami_event_to_amqp, NULL);
	if (!manager) {
		return AST_MODULE_LOAD_DECLINE;
	}

	if (!(stasis_app_sched_context = ast_sched_context_create())) {
		ast_log(LOG_ERROR, "failed to create scheduler context\n");
		/* unsubscribe from manager and sub */
		return AST_MODULE_LOAD_DECLINE;
	}

	if (ast_sched_start_thread(stasis_app_sched_context)) {
		ast_log(LOG_ERROR, "failed to start scheduler thread\n");
		/* unsubscribe from manager and sub */
		/* destroy context */
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_GLOBAL_SYMBOLS | AST_MODFLAG_LOAD_ORDER, "Send all Stasis messages to AMQP",
	.support_level = AST_MODULE_SUPPORT_CORE,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_APP_DEPEND,
	.requires = "res_stasis,res_amqp",
);
