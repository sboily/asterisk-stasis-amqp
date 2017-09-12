/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2013, Digium, Inc.
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
#include "asterisk/stasis_channels.h"
#include "asterisk/stasis_app.h"
#include "asterisk/stasis_message_router.h"
#include "asterisk/stasis_bridges.h"
#include "asterisk/ari.h"
#include "asterisk/time.h"
#include "asterisk/config_options.h"
#include "asterisk/manager.h"


#include "asterisk/amqp.h"

#define CONF_FILENAME "stasis_amqp.conf"

/*! Regular Stasis subscription */
static struct stasis_subscription *sub;
static struct stasis_subscription *manager;


static int setup_amqp(void);
static int stasis_amqp_channel_log(struct stasis_message *message);
static int publish_to_amqp(char *topic, char *json_msg);


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

/*!
 * \brief Subscription callback for all channel messages.
 * \param data Data pointer given when creating the subscription.
 * \param sub This subscription.
 * \param topic The topic the message was posted to. This is not necessarily the
 *              topic you subscribed to, since messages may be forwarded between
 *              topics.
 * \param message The message itself.
 */
static void send_channel_event_to_amqp(void *data, struct stasis_subscription *sub,
	struct stasis_message *message)
{
	if (stasis_subscription_final_message(sub, message)) {
		return;
	}

	stasis_amqp_channel_log(message);

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
	RAII_VAR(char *, stasis_msg, NULL, ast_json_free);
	RAII_VAR(struct ast_json *, manager_json, NULL, ast_json_unref);

	struct ast_manager_event_blob *manager_blob = stasis_message_to_ami(message);
	if (!manager_blob) {
	return;
	}

	manager_json = ast_json_pack(
		"{s:s, s:s}",
		"event_name", manager_blob->manager_event,
		"payload", manager_blob->extra_fields);

	if (!manager_json) {
		return;
	}

	publish_to_amqp("stasis.ami", ast_json_dump_string(manager_json));
}

/*!
 * \brief Channel handler for AMQP.
 *
 * \param message to Log.
 * \return 0 on success.
 * \return -1 on error.
 */
static int stasis_amqp_channel_log(struct stasis_message *message)
{
	RAII_VAR(char *, stasis_msg, NULL, ast_json_free);

	/*ast_log(LOG_ERROR, "%s\n", stasis_message_type_name(stasis_message_type(message)));*/
	stasis_msg = ast_json_dump_string_format(stasis_message_to_json(message, NULL), ast_ari_json_format());
	if (stasis_msg) {
		publish_to_amqp("stasis.channel", stasis_msg);
	}

	return -1;
}

static int publish_to_amqp(char *topic, char *json_msg)
{

	RAII_VAR(struct stasis_amqp_conf *, conf, NULL, ao2_cleanup);
	int res;

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
		amqp_cstring_bytes(json_msg));

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
	stasis_unsubscribe_and_join(sub);
	stasis_unsubscribe_and_join(manager);
	sub = NULL;
	manager = NULL;
	return 0;
}

static void stasis_app_message_handler(void *data, const char *app_name, struct ast_json *message)
{
	RAII_VAR(char *, str, NULL, ast_json_free);

	str = ast_json_dump_string_format(message, ast_ari_json_format());
	if (str == NULL) {
		ast_log(LOG_ERROR, "ARI: Failed to encode JSON object\n");
		return;
	}

	publish_to_amqp("stasis.app", str);

	return;
}

static int load_module(void)
{
	struct ao2_container *apps;
	struct ao2_iterator it_apps;
	char *app;

	if (!ast_module_check("res_amqp.so")) {
		if (ast_load_resource("res_amqp.so") != AST_MODULE_LOAD_SUCCESS) {
			ast_log(LOG_ERROR, "Cannot load res_amqp, so res_stasis_amqp cannot be loaded\n");
			return AST_MODULE_LOAD_DECLINE;
		}
	}

	if (load_config(0) != 0) {
		ast_log(LOG_WARNING, "Configuration failed to load\n");
		return AST_MODULE_LOAD_DECLINE;
	}

	/* Subscription to receive all of the messages from manager topic */
	manager = stasis_subscribe(ast_manager_get_topic(), send_ami_event_to_amqp, NULL);

	/* Subscription to receive all of the messages from channel topic */
	sub = stasis_subscribe(ast_channel_topic_all(), send_channel_event_to_amqp, NULL);

	/* Subscription to receive all of the messages from ari applications registered */
	apps = stasis_app_get_all();
	if (!apps) {
		ast_log(LOG_ERROR, "Unable to retrieve registered applications!\n");
		return AST_MODULE_LOAD_DECLINE;
	}

	it_apps = ao2_iterator_init(apps, 0);
	while ((app = ao2_iterator_next(&it_apps))) {
		stasis_app_register_all(app, &stasis_app_message_handler, NULL);
			ao2_ref(app, -1);
	}
	ao2_iterator_destroy(&it_apps);
	ao2_ref(apps, -1);

	if (!sub) {
		unload_module();
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Send all Stasis messages to AMQP",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.nonoptreq = "res_stasis_amqp"
);
