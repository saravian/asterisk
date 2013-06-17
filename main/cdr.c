/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
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

/*! \file
 *
 * \brief Call Detail Record API
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \note Includes code and algorithms from the Zapata library.
 *
 * \note We do a lot of checking here in the CDR code to try to be sure we don't ever let a CDR slip
 * through our fingers somehow.  If someone allocates a CDR, it must be completely handled normally
 * or a WARNING shall be logged, so that we can best keep track of any escape condition where the CDR
 * isn't properly generated and posted.
 */

/*! \li \ref cdr.c uses the configuration file \ref cdr.conf
 * \addtogroup configuration_file Configuration Files
 */

/*!
 * \page cdr.conf cdr.conf
 * \verbinclude cdr.conf.sample
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <signal.h>
#include <inttypes.h>

#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/cdr.h"
#include "asterisk/callerid.h"
#include "asterisk/manager.h"
#include "asterisk/causes.h"
#include "asterisk/linkedlists.h"
#include "asterisk/utils.h"
#include "asterisk/sched.h"
#include "asterisk/config.h"
#include "asterisk/cli.h"
#include "asterisk/stringfields.h"
#include "asterisk/data.h"
#include "asterisk/config_options.h"
#include "asterisk/json.h"
#include "asterisk/stasis.h"
#include "asterisk/stasis_channels.h"
#include "asterisk/stasis_bridging.h"
#include "asterisk/stasis_message_router.h"
#include "asterisk/astobj2.h"

/*** DOCUMENTATION
	<configInfo name="cdr" language="en_US">
		<synopsis>Call Detail Record configuration</synopsis>
		<description>
			<para>CDR is Call Detail Record, which provides logging services via a variety of
			pluggable backend modules. Detailed call information can be recorded to
			databases, files, etc. Useful for billing, fraud prevention, compliance with
			Sarbanes-Oxley aka The Enron Act, QOS evaluations, and more.</para>
		</description>
		<configFile name="cdr.conf">
			<configObject name="general">
				<synopsis>Global settings applied to the CDR engine.</synopsis>
				<configOption name="debug">
					<synopsis>Enable/disable verbose CDR debugging.</synopsis>
					<description><para>When set to <literal>True</literal>, verbose updates
					of changes in CDR information will be logged. Note that this is only
					of use when debugging CDR behavior.</para>
					</description>
				</configOption>
				<configOption name="enable">
					<synopsis>Enable/disable CDR logging.</synopsis>
					<description><para>Define whether or not to use CDR logging. Setting this to "no" will override
					any loading of backend CDR modules.  Default is "yes".</para>
					</description>
				</configOption>
				<configOption name="unanswered">
					<synopsis>Log calls that are never answered.</synopsis>
					<description><para>Define whether or not to log unanswered calls. Setting this to "yes" will
					report every attempt to ring a phone in dialing attempts, when it was not
					answered. For example, if you try to dial 3 extensions, and this option is "yes",
					you will get 3 CDR's, one for each phone that was rung. Some find this information horribly
					useless. Others find it very valuable. Note, in "yes" mode, you will see one CDR, with one of
					the call targets on one side, and the originating channel on the other, and then one CDR for
					each channel attempted. This may seem redundant, but cannot be helped.</para>
					<para>In brief, this option controls the reporting of unanswered calls which only have an A
					party. Calls which get offered to an outgoing line, but are unanswered, are still
					logged, and that is the intended behavior. (It also results in some B side CDRs being
					output, as they have the B side channel as their source channel, and no destination
					channel.)</para>
					</description>
				</configOption>
				<configOption name="congestion">
					<synopsis>Log congested calls.</synopsis>
					<description><para>Define whether or not to log congested calls. Setting this to "yes" will
					report each call that fails to complete due to congestion conditions.</para>
					</description>
				</configOption>
				<configOption name="endbeforehexten">
					<synopsis>End the CDR before executing the "h" extension</synopsis>
					<description><para>Normally, CDR's are not closed out until after all extensions are finished
					executing.  By enabling this option, the CDR will be ended before executing
					the <literal>h</literal> extension and hangup handlers so that CDR values such as <literal>end</literal> and
					<literal>"billsec"</literal> may be retrieved inside of this extension.
					The default value is "no".</para>
					</description>
				</configOption>
				<configOption name="initiatedseconds">
					<synopsis>Count microseconds for billsec purposes</synopsis>
					<description><para>Normally, the <literal>billsec</literal> field logged to the CDR backends
					is simply the end time (hangup time) minus the answer time in seconds. Internally,
					asterisk stores the time in terms of microseconds and seconds. By setting
					initiatedseconds to <literal>yes</literal>, you can force asterisk to report any seconds
					that were initiated (a sort of round up method). Technically, this is
					when the microsecond part of the end time is greater than the microsecond
					part of the answer time, then the billsec time is incremented one second.</para>
					</description>
				</configOption>
				<configOption name="batch">
					<synopsis>Submit CDRs to the backends for processing in batches</synopsis>
					<description><para>Define the CDR batch mode, where instead of posting the CDR at the end of
					every call, the data will be stored in a buffer to help alleviate load on the
					asterisk server.</para>
					<warning><para>Use of batch mode may result in data loss after unsafe asterisk termination,
					i.e., software crash, power failure, kill -9, etc.</para>
					</warning>
					</description>
				</configOption>
				<configOption name="size">
					<synopsis>The maximum number of CDRs to accumulate before triggering a batch</synopsis>
					<description><para>Define the maximum number of CDRs to accumulate in the buffer before posting
					them to the backend engines. batch must be set to <literal>yes</literal>.</para>
					</description>
				</configOption>
				<configOption name="time">
					<synopsis>The maximum time to accumulate CDRs before triggering a batch</synopsis>
					<description><para>Define the maximum time to accumulate CDRs before posting them in a batch to the
					backend engines. If this time limit is reached, then it will post the records, regardless of the value
					defined for size. batch must be set to <literal>yes</literal>.</para>
					<note><para>Time is expressed in seconds.</para></note>
					</description>
				</configOption>
				<configOption name="scheduleronly">
					<synopsis>Post batched CDRs on their own thread instead of the scheduler</synopsis>
					<description><para>The CDR engine uses the internal asterisk scheduler to determine when to post
					records.  Posting can either occur inside the scheduler thread, or a new
					thread can be spawned for the submission of every batch.  For small batches,
					it might be acceptable to just use the scheduler thread, so set this to <literal>yes</literal>.
					For large batches, say anything over size=10, a new thread is recommended, so
					set this to <literal>no</literal>.</para>
					</description>
				</configOption>
				<configOption name="safeshutdown">
					<synopsis>Block shutdown of Asterisk until CDRs are submitted</synopsis>
					<description><para>When shutting down asterisk, you can block until the CDRs are submitted.  If
					you don't, then data will likely be lost.  You can always check the size of
					the CDR batch buffer with the CLI <astcli>cdr status</astcli> command.  To enable blocking on
					submission of CDR data during asterisk shutdown, set this to <literal>yes</literal>.</para>
					</description>
				</configOption>
			</configObject>
		</configFile>
	</configInfo>
 ***/


#define DEFAULT_ENABLED "1"
#define DEFAULT_BATCHMODE "0"
#define DEFAULT_UNANSWERED "0"
#define DEFAULT_CONGESTION "0"
#define DEFAULT_END_BEFORE_H_EXTEN "0"
#define DEFAULT_INITIATED_SECONDS "0"

#define DEFAULT_BATCH_SIZE "100"
#define MAX_BATCH_SIZE 1000
#define DEFAULT_BATCH_TIME "300"
#define MAX_BATCH_TIME 86400
#define DEFAULT_BATCH_SCHEDULER_ONLY "0"
#define DEFAULT_BATCH_SAFE_SHUTDOWN "1"

#define CDR_DEBUG(mod_cfg, fmt, ...) \
	do { \
	if (ast_test_flag(&(mod_cfg)->general->settings, CDR_DEBUG)) { \
		ast_verb(1, (fmt), ##__VA_ARGS__); \
	} } while (0)

static void cdr_detach(struct ast_cdr *cdr);
static void cdr_submit_batch(int shutdown);

/*! \brief The configuration settings for this module */
struct module_config {
	struct ast_cdr_config *general;		/*< CDR global settings */
};

/*! \brief The container for the module configuration */
static AO2_GLOBAL_OBJ_STATIC(module_configs);

/*! \brief The type definition for general options */
static struct aco_type general_option = {
	.type = ACO_GLOBAL,
	.name = "general",
	.item_offset = offsetof(struct module_config, general),
	.category = "^general$",
	.category_match = ACO_WHITELIST,
};

static void *module_config_alloc(void);
static void module_config_destructor(void *obj);

/*! \brief The file definition */
static struct aco_file module_file_conf = {
	.filename = "cdr.conf",
	.skip_category = "(^csv$|^custom$|^manager$|^odbc$|^pgsql$|^radius$|^sqlite$|^tds$|^mysql$)",
	.types = ACO_TYPES(&general_option),
};

CONFIG_INFO_CORE("cdr", cfg_info, module_configs, module_config_alloc,
	.files = ACO_FILES(&module_file_conf),
);

static struct aco_type *general_options[] = ACO_TYPES(&general_option);

/*! \brief Dispose of a module config object */
static void module_config_destructor(void *obj)
{
	struct module_config *cfg = obj;

	if (!cfg) {
		return;
	}
	ao2_ref(cfg->general, -1);
}

/*! \brief Create a new module config object */
static void *module_config_alloc(void)
{
	struct module_config *mod_cfg;
	struct ast_cdr_config *cdr_config;

	mod_cfg = ao2_alloc(sizeof(*mod_cfg), module_config_destructor);
	if (!mod_cfg) {
		return NULL;
	}

	cdr_config = ao2_alloc(sizeof(*cdr_config), NULL);
	if (!cdr_config) {
		ao2_ref(cdr_config, -1);
		return NULL;
	}
	mod_cfg->general = cdr_config;

	return mod_cfg;
}

/*! \brief Registration object for CDR backends */
struct cdr_beitem {
	char name[20];
	char desc[80];
	ast_cdrbe be;
	AST_RWLIST_ENTRY(cdr_beitem) list;
};

/*! \brief List of registered backends */
static AST_RWLIST_HEAD_STATIC(be_list, cdr_beitem);

/*! \brief Queued CDR waiting to be batched */
struct cdr_batch_item {
	struct ast_cdr *cdr;
	struct cdr_batch_item *next;
};

/*! \brief The actual batch queue */
static struct cdr_batch {
	int size;
	struct cdr_batch_item *head;
	struct cdr_batch_item *tail;
} *batch = NULL;

/*! \brief The global sequence counter used for CDRs */
static int global_cdr_sequence =  0;

/*! \brief Scheduler items */
static struct ast_sched_context *sched;
static int cdr_sched = -1;
AST_MUTEX_DEFINE_STATIC(cdr_sched_lock);
static pthread_t cdr_thread = AST_PTHREADT_NULL;

/*! \brief Lock protecting modifications to the batch queue */
AST_MUTEX_DEFINE_STATIC(cdr_batch_lock);

/*! \brief These are used to wake up the CDR thread when there's work to do */
AST_MUTEX_DEFINE_STATIC(cdr_pending_lock);
static ast_cond_t cdr_pending_cond;

/*! \brief A container of the active CDRs indexed by Party A channel name */
static struct ao2_container *active_cdrs_by_channel;

/*! \brief A container of the active CDRs indexed by the bridge ID */
static struct ao2_container *active_cdrs_by_bridge;

/*! \brief Message router for stasis messages regarding channel state */
static struct stasis_message_router *stasis_router;

/*! \brief Our subscription for bridges */
static struct stasis_subscription *bridge_subscription;

/*! \brief Our subscription for channels */
static struct stasis_subscription *channel_subscription;

/*! \brief The parent topic for all topics we want to aggregate for CDRs */
static struct stasis_topic *cdr_topic;

struct cdr_object;

/*!
 * \brief A virtual table used for \ref cdr_object.
 *
 * Note that all functions are optional - if a subclass does not need an
 * implementation, it is safe to leave it NULL.
 */
struct cdr_object_fn_table {
	/*! \brief Name of the subclass */
	const char *name;

	/*!
	 * \brief An initialization function. This will be called automatically
	 * when a \ref cdr_object is switched to this type in
	 * \ref cdr_object_transition_state
	 *
	 * \param cdr The \ref cdr_object that was just transitioned
	 */
	void (* const init_function)(struct cdr_object *cdr);

	/*!
	 * \brief Process a Party A update for the \ref cdr_object
	 *
	 * \param cdr The \ref cdr_object to process the update
	 * \param snapshot The snapshot for the CDR's Party A
	 * \retval 0 the CDR handled the update or ignored it
	 * \retval 1 the CDR is finalized and a new one should be made to handle it
	 */
	int (* const process_party_a)(struct cdr_object *cdr,
			struct ast_channel_snapshot *snapshot);

	/*!
	 * \brief Process a Party B update for the \ref cdr_object
	 *
	 * \param cdr The \ref cdr_object to process the update
	 * \param snapshot The snapshot for the CDR's Party B
	 */
	void (* const process_party_b)(struct cdr_object *cdr,
			struct ast_channel_snapshot *snapshot);

	/*!
	 * \brief Process the beginning of a dial. A dial message implies one of two
	 * things:
	 * The \ref cdr_object's Party A has been originated
	 * The \ref cdr_object's Party A is dialing its Party B
	 *
	 * \param cdr The \ref cdr_object
	 * \param caller The originator of the dial attempt
	 * \param peer The destination of the dial attempt
	 *
	 * \retval 0 if the parties in the dial were handled by this CDR
	 * \retval 1 if the parties could not be handled by this CDR
	 */
	int (* const process_dial_begin)(struct cdr_object *cdr,
			struct ast_channel_snapshot *caller,
			struct ast_channel_snapshot *peer);

	/*!
	 * \brief Process the end of a dial. At the end of a dial, a CDR can be
	 * transitioned into one of two states - DialedPending
	 * (\ref dialed_pending_state_fn_table) or Finalized
	 * (\ref finalized_state_fn_table).
	 *
	 * \param cdr The \ref cdr_object
	 * \param caller The originator of the dial attempt
	 * \param peer the Destination of the dial attempt
	 * \param dial_status What happened
	 *
	 * \retval 0 if the parties in the dial were handled by this CDR
	 * \retval 1 if the parties could not be handled by this CDR
	 */
	int (* const process_dial_end)(struct cdr_object *cdr,
			struct ast_channel_snapshot *caller,
			struct ast_channel_snapshot *peer,
			const char *dial_status);

	/*!
	 * \brief Process the entering of a bridge by this CDR. The purpose of this
	 * callback is to have the CDR prepare itself for the bridge and attempt to
	 * find a valid Party B. The act of creating new CDRs based on the entering
	 * of this channel into the bridge is handled by the higher level message
	 * handler.
	 *
	 * \param cdr The \ref cdr_object
	 * \param bridge The bridge that the Party A just entered into
	 * \param channel The \ref ast_channel_snapshot for this CDR's Party A
	 *
	 * \retval 0 This CDR found a Party B for itself and updated it, or there
	 * was no Party B to find (we're all alone)
	 * \retval 1 This CDR couldn't find a Party B, and there were options
	 */
	int (* const process_bridge_enter)(struct cdr_object *cdr,
			struct ast_bridge_snapshot *bridge,
			struct ast_channel_snapshot *channel);

	/*!
	 * \brief Process the leaving of a bridge by this CDR.
	 *
	 * \param cdr The \ref cdr_object
	 * \param bridge The bridge that the Party A just left
	 * \param channel The \ref ast_channel_snapshot for this CDR's Party A
	 *
	 * \retval 0 This CDR left successfully
	 * \retval 1 Error
	 */
	int (* const process_bridge_leave)(struct cdr_object *cdr,
			struct ast_bridge_snapshot *bridge,
			struct ast_channel_snapshot *channel);
};

static int base_process_party_a(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot);
static int base_process_bridge_leave(struct cdr_object *cdr, struct ast_bridge_snapshot *bridge, struct ast_channel_snapshot *channel);
static int base_process_dial_end(struct cdr_object *cdr, struct ast_channel_snapshot *caller, struct ast_channel_snapshot *peer, const char *dial_status);

static void single_state_init_function(struct cdr_object *cdr);
static void single_state_process_party_b(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot);
static int single_state_process_dial_begin(struct cdr_object *cdr, struct ast_channel_snapshot *caller, struct ast_channel_snapshot *peer);
static int single_state_process_bridge_enter(struct cdr_object *cdr, struct ast_bridge_snapshot *bridge, struct ast_channel_snapshot *channel);

/*!
 * \brief The virtual table for the Single state.
 *
 * A \ref cdr_object starts off in this state. This represents a channel that
 * has no Party B information itself.
 *
 * A \ref cdr_object from this state can go into any of the following states:
 * * \ref dial_state_fn_table
 * * \ref bridge_state_fn_table
 * * \ref finalized_state_fn_table
 */
struct cdr_object_fn_table single_state_fn_table = {
	.name = "Single",
	.init_function = single_state_init_function,
	.process_party_a = base_process_party_a,
	.process_party_b = single_state_process_party_b,
	.process_dial_begin = single_state_process_dial_begin,
	.process_dial_end = base_process_dial_end,
	.process_bridge_enter = single_state_process_bridge_enter,
	.process_bridge_leave = base_process_bridge_leave,
};

static void dial_state_process_party_b(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot);
static int dial_state_process_dial_begin(struct cdr_object *cdr, struct ast_channel_snapshot *caller, struct ast_channel_snapshot *peer);
static int dial_state_process_dial_end(struct cdr_object *cdr, struct ast_channel_snapshot *caller, struct ast_channel_snapshot *peer, const char *dial_status);
static int dial_state_process_bridge_enter(struct cdr_object *cdr, struct ast_bridge_snapshot *bridge, struct ast_channel_snapshot *channel);

/*!
 * \brief The virtual table for the Dial state.
 *
 * A \ref cdr_object that has begun a dial operation. This state is entered when
 * the Party A for a CDR is determined to be dialing out to a Party B or when
 * a CDR is for an originated channel (in which case the Party A information is
 * the originated channel, and there is no Party B).
 *
 * A \ref cdr_object from this state can go in any of the following states:
 * * \ref dialed_pending_state_fn_table
 * * \ref bridge_state_fn_table
 * * \ref finalized_state_fn_table
 */
struct cdr_object_fn_table dial_state_fn_table = {
	.name = "Dial",
	.process_party_a = base_process_party_a,
	.process_party_b = dial_state_process_party_b,
	.process_dial_begin = dial_state_process_dial_begin,
	.process_dial_end = dial_state_process_dial_end,
	.process_bridge_enter = dial_state_process_bridge_enter,
	.process_bridge_leave = base_process_bridge_leave,
};

static int dialed_pending_state_process_party_a(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot);
static int dialed_pending_state_process_dial_begin(struct cdr_object *cdr, struct ast_channel_snapshot *caller, struct ast_channel_snapshot *peer);
static int dialed_pending_state_process_bridge_enter(struct cdr_object *cdr, struct ast_bridge_snapshot *bridge, struct ast_channel_snapshot *channel);

/*!
 * \brief The virtual table for the Dialed Pending state.
 *
 * A \ref cdr_object that has successfully finished a dial operation, but we
 * don't know what they're going to do yet. It's theoretically possible to dial
 * a party and then have that party not be bridged with the caller; likewise,
 * an origination can complete and the channel go off and execute dialplan. The
 * pending state acts as a bridge between either:
 * * Entering a bridge
 * * Getting a new CDR for new dialplan execution
 * * Switching from being originated to executing dialplan
 *
 * A \ref cdr_object from this state can go in any of the following states:
 * * \ref single_state_fn_table
 * * \ref dialed_pending_state_fn_table
 * * \ref bridge_state_fn_table
 * * \ref finalized_state_fn_table
 */
struct cdr_object_fn_table dialed_pending_state_fn_table = {
	.name = "DialedPending",
	.process_party_a = dialed_pending_state_process_party_a,
	.process_dial_begin = dialed_pending_state_process_dial_begin,
	.process_bridge_enter = dialed_pending_state_process_bridge_enter,
	.process_bridge_leave = base_process_bridge_leave,
};

static void bridge_state_process_party_b(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot);
static int bridge_state_process_bridge_leave(struct cdr_object *cdr, struct ast_bridge_snapshot *bridge, struct ast_channel_snapshot *channel);

/*!
 * \brief The virtual table for the Bridged state
 *
 * A \ref cdr_object enters this state when it receives notification that the
 * channel has entered a bridge.
 *
 * A \ref cdr_object from this state can go to:
 * * \ref finalized_state_fn_table
 * * \ref pending_state_fn_table
 */
struct cdr_object_fn_table bridge_state_fn_table = {
	.name = "Bridged",
	.process_party_a = base_process_party_a,
	.process_party_b = bridge_state_process_party_b,
	.process_bridge_leave = bridge_state_process_bridge_leave,
};

static void pending_state_init_function(struct cdr_object *cdr);
static int pending_state_process_party_a(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot);
static int pending_state_process_dial_begin(struct cdr_object *cdr, struct ast_channel_snapshot *caller, struct ast_channel_snapshot *peer);
static int pending_state_process_bridge_enter(struct cdr_object *cdr, struct ast_bridge_snapshot *bridge, struct ast_channel_snapshot *channel);

/*!
 * \brief The virtual table for the Pending state
 *
 * At certain times, we don't know where to go with the CDR. A good example is
 * when a channel leaves a bridge - we don't know if the channel is about to
 * be hung up; if it is about to go execute dialplan; dial someone; go into
 * another bridge, etc. At these times, the CDR goes into pending and observes
 * the messages that come in next to infer where the next logical place to go
 * is.
 *
 * In this state, a CDR can go anywhere!
 */
struct cdr_object_fn_table bridged_pending_state_fn_table = {
	.name = "Pending",
	.init_function = pending_state_init_function,
	.process_party_a = pending_state_process_party_a,
	.process_dial_begin = pending_state_process_dial_begin,
	.process_dial_end = base_process_dial_end,
	.process_bridge_enter = pending_state_process_bridge_enter,
	.process_bridge_leave = base_process_bridge_leave,
};

static void finalized_state_init_function(struct cdr_object *cdr);
static int finalized_state_process_party_a(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot);

/*!
 * \brief The virtual table for the finalized state.
 *
 * Once in the finalized state, the CDR is done. No modifications can be made
 * to the CDR.
 */
struct cdr_object_fn_table finalized_state_fn_table = {
	.name = "Finalized",
	.init_function = finalized_state_init_function,
	.process_party_a = finalized_state_process_party_a,
};

/*! \brief A wrapper object around a snapshot.
 * Fields that are mutable by the CDR engine are replicated here.
 */
struct cdr_object_snapshot {
	struct ast_channel_snapshot *snapshot;  /*!< The channel snapshot */
	char userfield[AST_MAX_USER_FIELD];     /*!< Userfield for the channel */
	unsigned int flags;                     /*!< Specific flags for this party */
	struct varshead variables;              /*!< CDR variables for the channel */
};

/*! \brief An in-memory representation of an active CDR */
struct cdr_object {
	struct cdr_object_snapshot party_a;     /*!< The Party A information */
	struct cdr_object_snapshot party_b;     /*!< The Party B information */
	struct cdr_object_fn_table *fn_table;   /*!< The current virtual table */

	enum ast_cdr_disposition disposition;   /*!< The disposition of the CDR */
	struct timeval start;                   /*!< When this CDR was created */
	struct timeval answer;                  /*!< Either when the channel was answered, or when the path between channels was established */
	struct timeval end;                     /*!< When this CDR was finalized */
	unsigned int sequence;                  /*!< A monotonically increasing number for each CDR */
	struct ast_flags flags;                 /*!< Flags on the CDR */
	AST_DECLARE_STRING_FIELDS(
		AST_STRING_FIELD(linkedid);         /*!< Linked ID. Cached here as it may change out from party A, which must be immutable */
		AST_STRING_FIELD(name);             /*!< Channel name of party A. Cached here as the party A address may change */
		AST_STRING_FIELD(bridge);           /*!< The bridge the party A happens to be in. */
		AST_STRING_FIELD(appl);             /*!< The last accepted application party A was in */
		AST_STRING_FIELD(data);             /*!< The data for the last accepted application party A was in */
	);
	struct cdr_object *next;                /*!< The next CDR object in the chain */
	struct cdr_object *last;                /*!< The last CDR object in the chain */
};

/*!
 * \brief Copy variables from one list to another
 * \param to_list destination
 * \param from_list source
 * \retval The number of copied variables
 */
static int copy_variables(struct varshead *to_list, struct varshead *from_list)
{
	struct ast_var_t *variables, *newvariable = NULL;
	const char *var, *val;
	int x = 0;

	AST_LIST_TRAVERSE(from_list, variables, entries) {
		if (variables &&
		    (var = ast_var_name(variables)) && (val = ast_var_value(variables)) &&
		    !ast_strlen_zero(var) && !ast_strlen_zero(val)) {
			newvariable = ast_var_assign(var, val);
			AST_LIST_INSERT_HEAD(to_list, newvariable, entries);
			x++;
		}
	}

	return x;
}

/*!
 * \brief Delete all variables from a variable list
 * \param headp The head pointer to the variable list to delete
 */
static void free_variables(struct varshead *headp)
{
	struct ast_var_t *vardata;

	while ((vardata = AST_LIST_REMOVE_HEAD(headp, entries))) {
		ast_var_delete(vardata);
	}
}

/*!
 * \brief Copy a snapshot and its details
 * \param dst The destination
 * \param src The source
 */
static void cdr_object_snapshot_copy(struct cdr_object_snapshot *dst, struct cdr_object_snapshot *src)
{
	if (dst->snapshot) {
		ao2_t_ref(dst->snapshot, -1, "release old snapshot during copy");
	}
	dst->snapshot = src->snapshot;
	ao2_t_ref(dst->snapshot, +1, "bump new snapshot during copy");
	strcpy(dst->userfield, src->userfield);
	dst->flags = src->flags;
	copy_variables(&dst->variables, &src->variables);
}

/*!
 * \brief Transition a \ref cdr_object to a new state
 * \param cdr The \ref cdr_object to transition
 * \param fn_table The \ref cdr_object_fn_table state to go to
 */
static void cdr_object_transition_state(struct cdr_object *cdr, struct cdr_object_fn_table *fn_table)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);

	CDR_DEBUG(mod_cfg, "%p - Transitioning CDR for %s from state %s to %s\n",
		cdr, cdr->party_a.snapshot->name,
		cdr->fn_table ? cdr->fn_table->name : "NONE", fn_table->name);
	cdr->fn_table = fn_table;
	if (cdr->fn_table->init_function) {
		cdr->fn_table->init_function(cdr);
	}
}
/*! \internal
 * \brief Hash function for containers of CDRs indexing by Party A name */
static int cdr_object_channel_hash_fn(const void *obj, const int flags)
{
	const struct cdr_object *cdr = obj;
	const char *name = (flags & OBJ_KEY) ? obj : cdr->name;
	return ast_str_case_hash(name);
}

/*! \internal
 * \brief Comparison function for containers of CDRs indexing by Party A name
 */
static int cdr_object_channel_cmp_fn(void *obj, void *arg, int flags)
{
	struct cdr_object *left = obj;
	struct cdr_object *right = arg;
	const char *match = (flags & OBJ_KEY) ? arg : right->name;
	return strcasecmp(left->name, match) ? 0 : (CMP_MATCH | CMP_STOP);
}

/*! \internal
 * \brief Hash function for containers of CDRs indexing by bridge ID
 */
static int cdr_object_bridge_hash_fn(const void *obj, const int flags)
{
	const struct cdr_object *cdr = obj;
	const char *id = (flags & OBJ_KEY) ? obj : cdr->bridge;
	return ast_str_case_hash(id);
}

/*! \internal
 * \brief Comparison function for containers of CDRs indexing by bridge. Note
 * that we expect there to be collisions, as a single bridge may have multiple
 * CDRs active at one point in time
 */
static int cdr_object_bridge_cmp_fn(void *obj, void *arg, int flags)
{
	struct cdr_object *left = obj;
	struct cdr_object *right = arg;
	struct cdr_object *it_cdr;
	const char *match = (flags & OBJ_KEY) ? arg : right->bridge;
	for (it_cdr = left; it_cdr; it_cdr = it_cdr->next) {
		if (!strcasecmp(it_cdr->bridge, match)) {
			return CMP_MATCH;
		}
	}
	return 0;
}

/*!
 * \brief \ref cdr_object Destructor
 */
static void cdr_object_dtor(void *obj)
{
	struct cdr_object *cdr = obj;
	struct ast_var_t *it_var;

	if (!cdr) {
		return;
	}

	ao2_cleanup(cdr->party_a.snapshot);
	ao2_cleanup(cdr->party_b.snapshot);
	while ((it_var = AST_LIST_REMOVE_HEAD(&cdr->party_a.variables, entries))) {
		ast_var_delete(it_var);
	}
	while ((it_var = AST_LIST_REMOVE_HEAD(&cdr->party_b.variables, entries))) {
		ast_var_delete(it_var);
	}
	ast_string_field_free_memory(cdr);

	if (cdr->next) {
		ao2_cleanup(cdr->next);
	}
}

/*!
 * \brief \ref cdr_object constructor
 * \param chan The \ref ast_channel_snapshot that is the CDR's Party A
 *
 * This implicitly sets the state of the newly created CDR to the Single state
 * (\ref single_state_fn_table)
 */
static struct cdr_object *cdr_object_alloc(struct ast_channel_snapshot *chan)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);
	struct cdr_object *cdr;

	ast_assert(chan != NULL);

	cdr = ao2_alloc(sizeof(*cdr), cdr_object_dtor);
	if (!cdr) {
		return NULL;
	}
	cdr->last = cdr;
	if (ast_string_field_init(cdr, 64)) {
		return NULL;
	}
	ast_string_field_set(cdr, name, chan->name);
	ast_string_field_set(cdr, linkedid, chan->linkedid);
	cdr->disposition = AST_CDR_NULL;
	cdr->sequence = ast_atomic_fetchadd_int(&global_cdr_sequence, +1);

	cdr->party_a.snapshot = chan;
	ao2_t_ref(cdr->party_a.snapshot, +1, "bump snapshot during CDR creation");

	CDR_DEBUG(mod_cfg, "%p - Created CDR for channel %s\n", cdr, chan->name);

	cdr_object_transition_state(cdr, &single_state_fn_table);

	return cdr;
}

/*!
 * \brief Create a new \ref cdr_object and append it to an existing chain
 * \param cdr The \ref cdr_object to append to
 */
static struct cdr_object *cdr_object_create_and_append(struct cdr_object *cdr)
{
	struct cdr_object *new_cdr;
	struct cdr_object *it_cdr;
	struct cdr_object *cdr_last;

	cdr_last = cdr->last;
	new_cdr = cdr_object_alloc(cdr_last->party_a.snapshot);
	if (!new_cdr) {
		return NULL;
	}
	new_cdr->disposition = AST_CDR_NULL;

	/* Copy over the linkedid, as it may have changed */
	ast_string_field_set(new_cdr, linkedid, cdr_last->linkedid);
	ast_string_field_set(new_cdr, appl, cdr_last->appl);
	ast_string_field_set(new_cdr, data, cdr_last->data);

	/* Copy over other Party A information */
	cdr_object_snapshot_copy(&new_cdr->party_a, &cdr_last->party_a);

	/* Append the CDR to the end of the list */
	for (it_cdr = cdr; it_cdr->next; it_cdr = it_cdr->next) {
		it_cdr->last = new_cdr;
	}
	it_cdr->last = new_cdr;
	it_cdr->next = new_cdr;

	return new_cdr;
}

/*!
 * \brief Return whether or not a \ref ast_channel_snapshot is for a channel
 * that was created as the result of a dial operation
 *
 * \retval 0 the channel was not created as the result of a dial
 * \retval 1 the channel was created as the result of a dial
 */
static int snapshot_is_dialed(struct ast_channel_snapshot *snapshot)
{
	return (ast_test_flag(&snapshot->flags, AST_FLAG_OUTGOING)
			&& !(ast_test_flag(&snapshot->flags, AST_FLAG_ORIGINATED)));
}

/*!
 * \brief Given two CDR snapshots, figure out who should be Party A for the
 * resulting CDR
 * \param left One of the snapshots
 * \param right The other snapshot
 * \retval The snapshot that won
 */
static struct cdr_object_snapshot *cdr_object_pick_party_a(struct cdr_object_snapshot *left, struct cdr_object_snapshot *right)
{
	/* Check whether or not the party is dialed. A dialed party is never the
	 * Party A with a party that was not dialed.
	 */
	if (!snapshot_is_dialed(left->snapshot) && snapshot_is_dialed(right->snapshot)) {
		return left;
	} else if (snapshot_is_dialed(left->snapshot) && !snapshot_is_dialed(right->snapshot)) {
		return right;
	}

	/* Try the Party A flag */
	if (ast_test_flag(left, AST_CDR_FLAG_PARTY_A) && !ast_test_flag(right, AST_CDR_FLAG_PARTY_A)) {
		return left;
	} else if (!ast_test_flag(right, AST_CDR_FLAG_PARTY_A) && ast_test_flag(right, AST_CDR_FLAG_PARTY_A)) {
		return right;
	}

	/* Neither party is dialed and neither has the Party A flag - defer to
	 * creation time */
	if (left->snapshot->creationtime.tv_sec < right->snapshot->creationtime.tv_sec) {
		return left;
	} else if (left->snapshot->creationtime.tv_sec > right->snapshot->creationtime.tv_sec) {
		return right;
	} else if (left->snapshot->creationtime.tv_usec > right->snapshot->creationtime.tv_usec) {
			return right;
	} else {
		/* Okay, fine, take the left one */
		return left;
	}
}

/*!
 * Compute the duration for a \ref cdr_object
 */
static long cdr_object_get_duration(struct cdr_object *cdr)
{
	if (ast_tvzero(cdr->end)) {
		return (long)(ast_tvdiff_ms(ast_tvnow(), cdr->start) / 1000);
	} else {
		return (long)(ast_tvdiff_ms(cdr->end, cdr->start) / 1000);
	}
}

/*!
 * \brief Compute the billsec for a \ref cdr_object
 */
static long cdr_object_get_billsec(struct cdr_object *cdr)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);
	long int ms;

	if (ast_tvzero(cdr->answer)) {
		return 0;
	}
	ms = ast_tvdiff_ms(cdr->end, cdr->answer);
	if (ast_test_flag(&mod_cfg->general->settings, CDR_INITIATED_SECONDS)
		&& (ms % 1000 >= 500)) {
		ms = (ms / 1000) + 1;
	} else {
		ms = ms / 1000;
	}

	return ms;
}

/*!
 * \brief Create a chain of \ref ast_cdr objects from a chain of \ref cdr_object
 * suitable for consumption by the registered CDR backends
 * \param cdr The \ref cdr_object to convert to a public record
 * \retval A chain of \ref ast_cdr objects on success
 * \retval NULL on failure
 */
static struct ast_cdr *cdr_object_create_public_records(struct cdr_object *cdr)
{
	struct ast_cdr *pub_cdr = NULL, *cdr_prev;
	struct ast_var_t *it_var, *it_copy_var;
	struct ast_channel_snapshot *party_a;
	struct ast_channel_snapshot *party_b;

	while (cdr) {
		struct ast_cdr *cdr_copy;

		/* Don't create records for CDRs where the party A was a dialed channel */
		if (snapshot_is_dialed(cdr->party_a.snapshot)) {
			cdr = cdr->next;
			continue;
		}

		cdr_copy = ast_calloc(1, sizeof(*cdr_copy));
		if (!cdr_copy) {
			ast_free(pub_cdr);
			return NULL;
		}

		party_a = cdr->party_a.snapshot;
		party_b = cdr->party_b.snapshot;

		/* Party A */
		ast_assert(party_a != NULL);
		ast_copy_string(cdr_copy->accountcode, party_a->accountcode, sizeof(cdr_copy->accountcode));
		cdr_copy->amaflags = party_a->amaflags;
		ast_copy_string(cdr_copy->channel, party_a->name, sizeof(cdr_copy->channel));
		ast_callerid_merge(cdr_copy->clid, sizeof(cdr_copy->clid), party_a->caller_name, party_a->caller_number, "");
		ast_copy_string(cdr_copy->src, party_a->caller_number, sizeof(cdr_copy->src));
		ast_copy_string(cdr_copy->uniqueid, party_a->uniqueid, sizeof(cdr_copy->uniqueid));
		ast_copy_string(cdr_copy->lastapp, cdr->appl, sizeof(cdr_copy->lastapp));
		ast_copy_string(cdr_copy->lastdata, cdr->data, sizeof(cdr_copy->lastdata));
		ast_copy_string(cdr_copy->dst, party_a->exten, sizeof(cdr_copy->dst));
		ast_copy_string(cdr_copy->dcontext, party_a->context, sizeof(cdr_copy->dcontext));

		/* Party B */
		if (party_b) {
			ast_copy_string(cdr_copy->dstchannel, party_b->name, sizeof(cdr_copy->dstchannel));
			ast_copy_string(cdr_copy->peeraccount, party_b->accountcode, sizeof(cdr_copy->peeraccount));
			if (!ast_strlen_zero(cdr->party_b.userfield)) {
				snprintf(cdr_copy->userfield, sizeof(cdr_copy->userfield), "%s;%s", cdr->party_a.userfield, cdr->party_b.userfield);
			}
		}
		if (ast_strlen_zero(cdr_copy->userfield) && !ast_strlen_zero(cdr->party_a.userfield)) {
			ast_copy_string(cdr_copy->userfield, cdr->party_a.userfield, sizeof(cdr_copy->userfield));
		}

		/* Timestamps/durations */
		cdr_copy->start = cdr->start;
		cdr_copy->answer = cdr->answer;
		cdr_copy->end = cdr->end;
		cdr_copy->billsec = cdr_object_get_billsec(cdr);
		cdr_copy->duration = cdr_object_get_duration(cdr);

		/* Flags and IDs */
		ast_copy_flags(cdr_copy, &cdr->flags, AST_FLAGS_ALL);
		ast_copy_string(cdr_copy->linkedid, cdr->linkedid, sizeof(cdr_copy->linkedid));
		cdr_copy->disposition = cdr->disposition;
		cdr_copy->sequence = cdr->sequence;

		/* Variables */
		copy_variables(&cdr_copy->varshead, &cdr->party_a.variables);
		AST_LIST_TRAVERSE(&cdr->party_b.variables, it_var, entries) {
			int found = 0;
			AST_LIST_TRAVERSE(&cdr_copy->varshead, it_copy_var, entries) {
				if (!strcmp(ast_var_name(it_var), ast_var_name(it_copy_var))) {
					found = 1;
					break;
				}
			}
			if (!found) {
				AST_LIST_INSERT_TAIL(&cdr_copy->varshead, ast_var_assign(ast_var_name(it_var),
						ast_var_value(it_var)), entries);
			}
		}

		if (!pub_cdr) {
			pub_cdr = cdr_copy;
			cdr_prev = pub_cdr;
		} else {
			cdr_prev->next = cdr_copy;
			cdr_prev = cdr_copy;
		}
		cdr = cdr->next;
	}

	return pub_cdr;
}

/*!
 * \brief Dispatch a CDR.
 * \param cdr The \ref cdr_object to dispatch
 *
 * This will create a \ref ast_cdr object and publish it to the various backends
 */
static void cdr_object_dispatch(struct cdr_object *cdr)
{
	RAII_VAR(struct module_config *, mod_cfg,
			ao2_global_obj_ref(module_configs), ao2_cleanup);
	struct ast_cdr *pub_cdr;

	CDR_DEBUG(mod_cfg, "%p - Dispatching CDR for Party A %s, Party B %s\n", cdr,
			cdr->party_a.snapshot->name,
			cdr->party_b.snapshot ? cdr->party_b.snapshot->name : "<none>");
	pub_cdr = cdr_object_create_public_records(cdr);
	cdr_detach(pub_cdr);
}

/*!
 * \brief Set the disposition on a \ref cdr_object based on a hangupcause code
 * \param cdr The \ref cdr_object
 * \param hangupcause The Asterisk hangup cause code
 */
static void cdr_object_set_disposition(struct cdr_object *cdr, int hangupcause)
{
	RAII_VAR(struct module_config *, mod_cfg,
			ao2_global_obj_ref(module_configs), ao2_cleanup);

	/* Change the disposition based on the hang up cause */
	switch (hangupcause) {
	case AST_CAUSE_BUSY:
		cdr->disposition = AST_CDR_BUSY;
		break;
	case AST_CAUSE_CONGESTION:
		if (!ast_test_flag(&mod_cfg->general->settings, CDR_CONGESTION)) {
			cdr->disposition = AST_CDR_FAILED;
		} else {
			cdr->disposition = AST_CDR_CONGESTION;
		}
		break;
	case AST_CAUSE_NO_ROUTE_DESTINATION:
	case AST_CAUSE_UNREGISTERED:
		cdr->disposition = AST_CDR_FAILED;
		break;
	case AST_CAUSE_NORMAL_CLEARING:
	case AST_CAUSE_NO_ANSWER:
		cdr->disposition = AST_CDR_NOANSWER;
		break;
	default:
		break;
	}
}

/*!
 * \brief Finalize a CDR.
 *
 * This function is safe to call multiple times. Note that you can call this
 * explicitly before going to the finalized state if there's a chance the CDR
 * will be re-activated, in which case the \ref cdr_object's end time should be
 * cleared. This function is implicitly called when a CDR transitions to the
 * finalized state and right before it is dispatched
 *
 * \param cdr_object The CDR to finalize
 */
static void cdr_object_finalize(struct cdr_object *cdr)
{
	RAII_VAR(struct module_config *, mod_cfg,
			ao2_global_obj_ref(module_configs), ao2_cleanup);

	if (!ast_tvzero(cdr->end)) {
		return;
	}
	cdr->end = ast_tvnow();

	if (cdr->disposition == AST_CDR_NULL) {
		if (!ast_tvzero(cdr->answer)) {
			cdr->disposition = AST_CDR_ANSWERED;
		} else if (cdr->party_a.snapshot->hangupcause) {
			cdr_object_set_disposition(cdr, cdr->party_a.snapshot->hangupcause);
		} else if (cdr->party_b.snapshot && cdr->party_b.snapshot->hangupcause) {
			cdr_object_set_disposition(cdr, cdr->party_b.snapshot->hangupcause);
		} else {
			cdr->disposition = AST_CDR_FAILED;
		}
	}

	ast_debug(1, "Finalized CDR for %s - start %ld.%ld answer %ld.%ld end %ld.%ld dispo %s\n",
			cdr->party_a.snapshot->name,
			cdr->start.tv_sec,
			cdr->start.tv_usec,
			cdr->answer.tv_sec,
			cdr->answer.tv_usec,
			cdr->end.tv_sec,
			cdr->end.tv_usec,
			ast_cdr_disp2str(cdr->disposition));
}

/*!
 * \brief Check to see if a CDR needs to move to the finalized state because
 * its Party A hungup.
 */
static void cdr_object_check_party_a_hangup(struct cdr_object *cdr)
{
	if (ast_test_flag(&cdr->party_a.snapshot->flags, AST_FLAG_ZOMBIE)
		&& cdr->fn_table != &finalized_state_fn_table) {
		cdr_object_transition_state(cdr, &finalized_state_fn_table);
	}
}

/*!
 * \brief Check to see if a CDR needs to be answered based on its Party A.
 * Note that this is safe to call as much as you want - we won't answer twice
 */
static void cdr_object_check_party_a_answer(struct cdr_object *cdr) {
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);

	if (cdr->party_a.snapshot->state == AST_STATE_UP && ast_tvzero(cdr->answer)) {
		cdr->answer = ast_tvnow();
		CDR_DEBUG(mod_cfg, "%p - Set answered time to %ld.%ld\n", cdr,
			cdr->answer.tv_sec,
			cdr->answer.tv_usec);
	}
}

/*!
 * \internal
 * \brief Set a variable on a CDR object
 *
 * \param headp The header pointer to the variable to set
 * \param name The name of the variable
 * \param value The value of the variable
 *
 * CDRs that are in a hungup state cannot have their variables set.
 */
static void set_variable(struct varshead *headp, const char *name, const char *value)
{
	struct ast_var_t *newvariable;

	AST_LIST_TRAVERSE_SAFE_BEGIN(headp, newvariable, entries) {
		if (!strcasecmp(ast_var_name(newvariable), name)) {
			AST_LIST_REMOVE_CURRENT(entries);
			ast_var_delete(newvariable);
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;

	if (value) {
		newvariable = ast_var_assign(name, value);
		AST_LIST_INSERT_HEAD(headp, newvariable, entries);
	}
}

/* \brief Set Caller ID information on a CDR */
static void cdr_object_update_cid(struct cdr_object_snapshot *old_snapshot, struct ast_channel_snapshot *new_snapshot)
{
	if (!old_snapshot->snapshot) {
		set_variable(&old_snapshot->variables, "dnid", new_snapshot->caller_dnid);
		set_variable(&old_snapshot->variables, "callingsubaddr", new_snapshot->caller_subaddr);
		set_variable(&old_snapshot->variables, "calledsubaddr", new_snapshot->dialed_subaddr);
		return;
	}
	if (!strcmp(old_snapshot->snapshot->caller_dnid, new_snapshot->caller_dnid)) {
		set_variable(&old_snapshot->variables, "dnid", new_snapshot->caller_dnid);
	}
	if (!strcmp(old_snapshot->snapshot->caller_subaddr, new_snapshot->caller_subaddr)) {
		set_variable(&old_snapshot->variables, "callingsubaddr", new_snapshot->caller_subaddr);
	}
	if (!strcmp(old_snapshot->snapshot->dialed_subaddr, new_snapshot->dialed_subaddr)) {
		set_variable(&old_snapshot->variables, "calledsubaddr", new_snapshot->dialed_subaddr);
	}
}

/*!
 * \brief Swap an old \ref cdr_object_snapshot's \ref ast_channel_snapshot for
 * a new \ref ast_channel_snapshot
 * \param old_snapshot The old \ref cdr_object_snapshot
 * \param new_snapshot The new \ref ast_channel_snapshot for old_snapshot
 */
static void cdr_object_swap_snapshot(struct cdr_object_snapshot *old_snapshot,
		struct ast_channel_snapshot *new_snapshot)
{
	cdr_object_update_cid(old_snapshot, new_snapshot);
	if (old_snapshot->snapshot) {
		ao2_t_ref(old_snapshot->snapshot, -1, "Drop ref for swap");
	}
	ao2_t_ref(new_snapshot, +1, "Bump ref for swap");
	old_snapshot->snapshot = new_snapshot;
}

/* BASE METHOD IMPLEMENTATIONS */

static int base_process_party_a(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);

	ast_assert(strcmp(snapshot->name, cdr->party_a.snapshot->name) == 0);
	cdr_object_swap_snapshot(&cdr->party_a, snapshot);

	/* When Party A is originated to an application and the application exits, the stack
	 * will attempt to clear the application and restore the dummy originate application
	 * of "AppDialX". Prevent that, and any other application changes we might not want
	 * here.
	 */
	if (!ast_strlen_zero(snapshot->appl) && (strncasecmp(snapshot->appl, "appdial", 7) || ast_strlen_zero(cdr->appl))) {
		ast_string_field_set(cdr, appl, snapshot->appl);
		ast_string_field_set(cdr, data, snapshot->data);
	}

	ast_string_field_set(cdr, linkedid, snapshot->linkedid);
	cdr_object_check_party_a_answer(cdr);
	cdr_object_check_party_a_hangup(cdr);

	return 0;
}

static int base_process_bridge_leave(struct cdr_object *cdr, struct ast_bridge_snapshot *bridge, struct ast_channel_snapshot *channel)
{
	/* In general, most things shouldn't get a bridge leave */
	ast_assert(0);
	return 1;
}

static int base_process_dial_end(struct cdr_object *cdr, struct ast_channel_snapshot *caller, struct ast_channel_snapshot *peer, const char *dial_status)
{
	/* In general, most things shouldn't get a dial end. */
	ast_assert(0);
	return 0;
}

/* SINGLE STATE */

static void single_state_init_function(struct cdr_object *cdr) {
	cdr->start = ast_tvnow();
	cdr_object_check_party_a_answer(cdr);
}

static void single_state_process_party_b(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot)
{
	/* This should never happen! */
	ast_assert(cdr->party_b.snapshot == NULL);
	ast_assert(0);
	return;
}

static int single_state_process_dial_begin(struct cdr_object *cdr, struct ast_channel_snapshot *caller, struct ast_channel_snapshot *peer)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);

	if (caller && !strcmp(cdr->party_a.snapshot->name, caller->name)) {
		cdr_object_swap_snapshot(&cdr->party_a, caller);
		CDR_DEBUG(mod_cfg, "%p - Updated Party A %s snapshot\n", cdr,
				cdr->party_a.snapshot->name);
		cdr_object_swap_snapshot(&cdr->party_b, peer);
		CDR_DEBUG(mod_cfg, "%p - Updated Party B %s snapshot\n", cdr,
				cdr->party_b.snapshot->name);
	} else if (!strcmp(cdr->party_a.snapshot->name, peer->name)) {
		/* We're the entity being dialed, i.e., outbound origination */
		cdr_object_swap_snapshot(&cdr->party_a, peer);
		CDR_DEBUG(mod_cfg, "%p - Updated Party A %s snapshot\n", cdr,
				cdr->party_a.snapshot->name);
	}

	cdr_object_transition_state(cdr, &dial_state_fn_table);
	return 0;
}

/*!
 * \brief Handle a comparison between our \ref cdr_object and a \ref cdr_object
 * already in the bridge while in the Single state. The goal of this is to find
 * a Party B for our CDR.
 *
 * \param cdr Our \ref cdr_object in the Single state
 * \param cand_cdr The \ref cdr_object already in the Bridge state
 *
 * \retval 0 The cand_cdr had a Party A or Party B that we could use as our
 * Party B
 * \retval 1 No party in the cand_cdr could be used as our Party B
 */
static int single_state_bridge_enter_comparison(struct cdr_object *cdr,
		struct cdr_object *cand_cdr)
{
	struct cdr_object_snapshot *party_a;

	/* Try the candidate CDR's Party A first */
	party_a = cdr_object_pick_party_a(&cdr->party_a, &cand_cdr->party_a);
	if (!strcmp(party_a->snapshot->name, cdr->party_a.snapshot->name)) {
		cdr_object_snapshot_copy(&cdr->party_b, &cand_cdr->party_a);
		if (!cand_cdr->party_b.snapshot) {
			/* We just stole them - finalize their CDR. Note that this won't
			 * transition their state, it just sets the end time and the
			 * disposition - if we need to re-activate them later, we can.
			 */
			cdr_object_finalize(cand_cdr);
		}
		return 0;
	}

	/* Try their Party B */
	if (!cand_cdr->party_b.snapshot) {
		return 1;
	}
	party_a = cdr_object_pick_party_a(&cdr->party_a, &cand_cdr->party_b);
	if (!strcmp(party_a->snapshot->name, cdr->party_a.snapshot->name)) {
		cdr_object_snapshot_copy(&cdr->party_b, &cand_cdr->party_b);
		return 0;
	}

	return 1;
}

static int single_state_process_bridge_enter(struct cdr_object *cdr, struct ast_bridge_snapshot *bridge, struct ast_channel_snapshot *channel)
{
	struct ao2_iterator *it_cdrs;
	struct cdr_object *cand_cdr_master;
	char *bridge_id = ast_strdupa(bridge->uniqueid);
	int success = 1;

	ast_string_field_set(cdr, bridge, bridge->uniqueid);

	/* Get parties in the bridge */
	it_cdrs = ao2_callback(active_cdrs_by_bridge, OBJ_MULTIPLE | OBJ_KEY,
			cdr_object_bridge_cmp_fn, bridge_id);
	if (!it_cdrs) {
		/* No one in the bridge yet! */
		cdr_object_transition_state(cdr, &bridge_state_fn_table);
		return 0;
	}

	while ((cand_cdr_master = ao2_iterator_next(it_cdrs))) {
		struct cdr_object *cand_cdr;

		ao2_lock(cand_cdr_master);
		for (cand_cdr = cand_cdr_master; cand_cdr; cand_cdr = cand_cdr->next) {
			/* Skip any records that are not in a bridge or in this bridge.
			 * I'm not sure how that would happen, but it pays to be careful. */
			if (cand_cdr->fn_table != &bridge_state_fn_table ||
					strcmp(cdr->bridge, cand_cdr->bridge)) {
				continue;
			}

			if (single_state_bridge_enter_comparison(cdr, cand_cdr)) {
				continue;
			}
			/* We successfully got a party B - break out */
			success = 0;
			break;
		}
		ao2_unlock(cand_cdr_master);
		ao2_t_ref(cand_cdr_master, -1, "Drop iterator reference");
	}
	ao2_iterator_destroy(it_cdrs);

	/* We always transition state, even if we didn't get a peer */
	cdr_object_transition_state(cdr, &bridge_state_fn_table);

	/* Success implies that we have a Party B */
	return success;
}

/* DIAL STATE */

static void dial_state_process_party_b(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot)
{
	ast_assert(snapshot != NULL);

	if (!cdr->party_b.snapshot || strcmp(cdr->party_b.snapshot->name, snapshot->name)) {
		return;
	}
	cdr_object_swap_snapshot(&cdr->party_b, snapshot);

	/* If party B hangs up, finalize this CDR */
	if (ast_test_flag(&cdr->party_b.snapshot->flags, AST_FLAG_ZOMBIE)) {
		cdr_object_transition_state(cdr, &finalized_state_fn_table);
	}
}

static int dial_state_process_dial_begin(struct cdr_object *cdr, struct ast_channel_snapshot *caller, struct ast_channel_snapshot *peer)
{
	/* Don't process a begin dial here. A party A already in the dial state will
	 * who receives a dial begin for something else will be handled by the
	 * message router callback and will add a new CDR for the party A */
	return 1;
}

/*! \internal \brief Convert a dial status to a CDR disposition */
static enum ast_cdr_disposition dial_status_to_disposition(const char *dial_status)
{
	RAII_VAR(struct module_config *, mod_cfg,
		ao2_global_obj_ref(module_configs), ao2_cleanup);

	if (!strcmp(dial_status, "ANSWER")) {
		return AST_CDR_ANSWERED;
	} else if (!strcmp(dial_status, "BUSY")) {
		return AST_CDR_BUSY;
	} else if (!strcmp(dial_status, "CANCEL") || !strcmp(dial_status, "NOANSWER")) {
		return AST_CDR_NOANSWER;
	} else if (!strcmp(dial_status, "CONGESTION")) {
		if (!ast_test_flag(&mod_cfg->general->settings, CDR_CONGESTION)) {
			return AST_CDR_FAILED;
		} else {
			return AST_CDR_CONGESTION;
		}
	} else if (!strcmp(dial_status, "FAILED")) {
		return AST_CDR_FAILED;
	}
	return AST_CDR_FAILED;
}

static int dial_state_process_dial_end(struct cdr_object *cdr, struct ast_channel_snapshot *caller, struct ast_channel_snapshot *peer, const char *dial_status)
{
	RAII_VAR(struct module_config *, mod_cfg,
			ao2_global_obj_ref(module_configs), ao2_cleanup);
	struct ast_channel_snapshot *party_a;

	if (caller) {
		party_a = caller;
	} else {
		party_a = peer;
	}
	ast_assert(!strcmp(cdr->party_a.snapshot->name, party_a->name));
	cdr_object_swap_snapshot(&cdr->party_a, party_a);

	if (cdr->party_b.snapshot) {
		if (strcmp(cdr->party_b.snapshot->name, peer->name)) {
			/* Not the status for this CDR - defer back to the message router */
			return 1;
		}
		cdr_object_swap_snapshot(&cdr->party_b, peer);
	}

	/* Set the disposition based on the dial string. */
	cdr->disposition = dial_status_to_disposition(dial_status);
	if (cdr->disposition == AST_CDR_ANSWERED) {
		/* Switch to dial pending to wait and see what the caller does */
		cdr_object_transition_state(cdr, &dialed_pending_state_fn_table);
	} else {
		cdr_object_transition_state(cdr, &finalized_state_fn_table);
	}

	return 0;
}

static int dial_state_process_bridge_enter(struct cdr_object *cdr, struct ast_bridge_snapshot *bridge, struct ast_channel_snapshot *channel)
{
	struct ao2_iterator *it_cdrs;
	char *bridge_id = ast_strdupa(bridge->uniqueid);
	struct cdr_object *cand_cdr_master;
	int success = 1;

	ast_string_field_set(cdr, bridge, bridge->uniqueid);

	/* Get parties in the bridge */
	it_cdrs = ao2_callback(active_cdrs_by_bridge, OBJ_MULTIPLE | OBJ_KEY,
			cdr_object_bridge_cmp_fn, bridge_id);
	if (!it_cdrs) {
		/* No one in the bridge yet! */
		cdr_object_transition_state(cdr, &bridge_state_fn_table);
		return 0;
	}

	while ((cand_cdr_master = ao2_iterator_next(it_cdrs))) {
		struct cdr_object *cand_cdr;

		ao2_lock(cand_cdr_master);
		for (cand_cdr = cand_cdr_master; cand_cdr; cand_cdr = cand_cdr->next) {
			/* Skip any records that are not in a bridge or in this bridge.
			 * I'm not sure how that would happen, but it pays to be careful. */
			if (cand_cdr->fn_table != &bridge_state_fn_table ||
					strcmp(cdr->bridge, cand_cdr->bridge)) {
				continue;
			}

			/* Skip any records that aren't our Party B */
			if (strcmp(cdr->party_b.snapshot->name, cand_cdr->party_a.snapshot->name)) {
				continue;
			}

			cdr_object_snapshot_copy(&cdr->party_b, &cand_cdr->party_a);
			/* If they have a Party B, they joined up with someone else as their
			 * Party A. Don't finalize them as they're active. Otherwise, we
			 * have stolen them so they need to be finalized.
			 */
			if (!cand_cdr->party_b.snapshot) {
				cdr_object_finalize(cand_cdr);
			}
			success = 0;
			break;
		}
		ao2_unlock(cand_cdr_master);
		ao2_t_ref(cand_cdr_master, -1, "Drop iterator reference");
	}
	ao2_iterator_destroy(it_cdrs);

	/* We always transition state, even if we didn't get a peer */
	cdr_object_transition_state(cdr, &bridge_state_fn_table);

	/* Success implies that we have a Party B */
	return success;
}

/* DIALED PENDING STATE */

static int dialed_pending_state_process_party_a(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot)
{
	/* If we get a CEP change, we're executing dialplan. If we have a Party B
	 * that means we need a new CDR; otherwise, switch us over to single.
	 */
	if (strcmp(snapshot->context, cdr->party_a.snapshot->context)
		|| strcmp(snapshot->exten, cdr->party_a.snapshot->exten)
		|| snapshot->priority != cdr->party_a.snapshot->priority
		|| strcmp(snapshot->appl, cdr->party_a.snapshot->appl)) {
		if (cdr->party_b.snapshot) {
			cdr_object_transition_state(cdr, &finalized_state_fn_table);
			cdr->fn_table->process_party_a(cdr, snapshot);
			return 1;
		} else {
			cdr_object_transition_state(cdr, &single_state_fn_table);
			cdr->fn_table->process_party_a(cdr, snapshot);
			return 0;
		}
	}
	base_process_party_a(cdr, snapshot);
	return 0;
}

static int dialed_pending_state_process_bridge_enter(struct cdr_object *cdr, struct ast_bridge_snapshot *bridge, struct ast_channel_snapshot *channel)
{
	cdr_object_transition_state(cdr, &dial_state_fn_table);
	return cdr->fn_table->process_bridge_enter(cdr, bridge, channel);
}

static int dialed_pending_state_process_dial_begin(struct cdr_object *cdr, struct ast_channel_snapshot *caller, struct ast_channel_snapshot *peer)
{
	struct cdr_object *new_cdr;

	cdr_object_transition_state(cdr, &finalized_state_fn_table);
	new_cdr = cdr_object_create_and_append(cdr);
	cdr_object_transition_state(cdr, &single_state_fn_table);
	return new_cdr->fn_table->process_dial_begin(cdr, caller, peer);
}

/* BRIDGE STATE */

static void bridge_state_process_party_b(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot)
{
	if (!cdr->party_b.snapshot || strcmp(cdr->party_b.snapshot->name, snapshot->name)) {
		return;
	}
	cdr_object_swap_snapshot(&cdr->party_b, snapshot);

	/* If party B hangs up, finalize this CDR */
	if (ast_test_flag(&cdr->party_b.snapshot->flags, AST_FLAG_ZOMBIE)) {
		cdr_object_transition_state(cdr, &finalized_state_fn_table);
	}
}

static int bridge_state_process_bridge_leave(struct cdr_object *cdr, struct ast_bridge_snapshot *bridge, struct ast_channel_snapshot *channel)
{
	if (strcmp(cdr->bridge, bridge->uniqueid)) {
		return 1;
	}
	if (strcmp(cdr->party_a.snapshot->name, channel->name)
			&& cdr->party_b.snapshot
			&& strcmp(cdr->party_b.snapshot->name, channel->name)) {
		return 1;
	}
	cdr_object_transition_state(cdr, &finalized_state_fn_table);

	return 0;
}

/* PENDING STATE */

static void pending_state_init_function(struct cdr_object *cdr)
{
	ast_cdr_set_property(cdr->name, AST_CDR_FLAG_DISABLE);
}

static int pending_state_process_party_a(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot)
{
	if (ast_test_flag(&snapshot->flags, AST_FLAG_ZOMBIE)) {
		return 0;
	}

	/* Ignore if we don't get a CEP change */
	if (!strcmp(snapshot->context, cdr->party_a.snapshot->context)
		&& !strcmp(snapshot->exten, cdr->party_a.snapshot->exten)
		&& snapshot->priority == cdr->party_a.snapshot->priority) {
		return 0;
	}

	cdr_object_transition_state(cdr, &single_state_fn_table);
	ast_cdr_clear_property(cdr->name, AST_CDR_FLAG_DISABLE);
	cdr->fn_table->process_party_a(cdr, snapshot);
	return 0;
}

static int pending_state_process_dial_begin(struct cdr_object *cdr, struct ast_channel_snapshot *caller, struct ast_channel_snapshot *peer)
{
	cdr_object_transition_state(cdr, &single_state_fn_table);
	ast_cdr_clear_property(cdr->name, AST_CDR_FLAG_DISABLE);
	return cdr->fn_table->process_dial_begin(cdr, caller, peer);
}

static int pending_state_process_bridge_enter(struct cdr_object *cdr, struct ast_bridge_snapshot *bridge, struct ast_channel_snapshot *channel)
{
	cdr_object_transition_state(cdr, &single_state_fn_table);
	ast_cdr_clear_property(cdr->name, AST_CDR_FLAG_DISABLE);
	return cdr->fn_table->process_bridge_enter(cdr, bridge, channel);
}

/* FINALIZED STATE */

static void finalized_state_init_function(struct cdr_object *cdr)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);

	if (!ast_test_flag(&mod_cfg->general->settings, CDR_END_BEFORE_H_EXTEN)) {
		return;
	}

	cdr_object_finalize(cdr);
}

static int finalized_state_process_party_a(struct cdr_object *cdr, struct ast_channel_snapshot *snapshot)
{
	if (ast_test_flag(&cdr->party_a.snapshot->flags, AST_FLAG_ZOMBIE)) {
		cdr_object_finalize(cdr);
	}

	/* Indicate that, if possible, we should get a new CDR */
	return 1;
}

/* TOPIC ROUTER CALLBACKS */

/*!
 * \brief Handler for Stasis-Core dial messages
 * \param data Passed on
 * \param sub The stasis subscription for this message callback
 * \param topic The topic this message was published for
 * \param message The message
 */
static void handle_dial_message(void *data, struct stasis_subscription *sub, struct stasis_topic *topic, struct stasis_message *message)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);
	RAII_VAR(struct cdr_object *, cdr_caller, NULL, ao2_cleanup);
	RAII_VAR(struct cdr_object *, cdr_peer, NULL, ao2_cleanup);
	struct cdr_object *cdr;
	struct ast_multi_channel_blob *payload = stasis_message_data(message);
	struct ast_channel_snapshot *caller;
	struct ast_channel_snapshot *peer;
	struct cdr_object_snapshot *party_a;
	struct cdr_object_snapshot *party_b;
	struct cdr_object *it_cdr;
	struct ast_json *dial_status_blob;
	const char *dial_status = NULL;
	int res = 1;

	CDR_DEBUG(mod_cfg, "Dial message: %u.%08u\n", (unsigned int)stasis_message_timestamp(message)->tv_sec, (unsigned int)stasis_message_timestamp(message)->tv_usec);
	ast_assert(payload != NULL);

	caller = ast_multi_channel_blob_get_channel(payload, "caller");
	peer = ast_multi_channel_blob_get_channel(payload, "peer");
	if (!peer && !caller) {
		return;
	}
	dial_status_blob = ast_json_object_get(ast_multi_channel_blob_get_json(payload), "dialstatus");
	if (dial_status_blob) {
		dial_status = ast_json_string_get(dial_status_blob);
	}

	/* Figure out who is running this show */
	if (caller) {
		cdr_caller = ao2_find(active_cdrs_by_channel, caller->name, OBJ_KEY);
	}
	if (peer) {
		cdr_peer = ao2_find(active_cdrs_by_channel, peer->name, OBJ_KEY);
	}
	if (cdr_caller && cdr_peer) {
		party_a = cdr_object_pick_party_a(&cdr_caller->party_a, &cdr_peer->party_a);
		if (!strcmp(party_a->snapshot->name, cdr_caller->party_a.snapshot->name)) {
			cdr = cdr_caller;
			party_b = &cdr_peer->party_a;
		} else {
			cdr = cdr_peer;
			party_b = &cdr_caller->party_a;
		}
	} else if (cdr_caller) {
		cdr = cdr_caller;
		party_a = &cdr_caller->party_a;
		party_b = NULL;
	} else if (cdr_peer) {
		cdr = cdr_peer;
		party_a = NULL;
		party_b = &cdr_peer->party_a;
	} else {
		return;
	}

	ao2_lock(cdr);
	for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
		if (ast_strlen_zero(dial_status)) {
			if (!it_cdr->fn_table->process_dial_begin) {
				continue;
			}
			CDR_DEBUG(mod_cfg, "%p - Processing Dial Begin message for channel %s, peer %s\n",
					cdr,
					party_a ? party_a->snapshot->name : "(none)",
					party_b ? party_b->snapshot->name : "(none)");
			res &= it_cdr->fn_table->process_dial_begin(it_cdr,
					party_a ? party_a->snapshot : NULL,
					party_b ? party_b->snapshot : NULL);
		} else {
			if (!it_cdr->fn_table->process_dial_end) {
				continue;
			}
			CDR_DEBUG(mod_cfg, "%p - Processing Dial End message for channel %s, peer %s\n",
					cdr,
					party_a ? party_a->snapshot->name : "(none)",
					party_b ? party_b->snapshot->name : "(none)");
			it_cdr->fn_table->process_dial_end(it_cdr,
					party_a ? party_a->snapshot : NULL,
					party_b ? party_b->snapshot : NULL,
					dial_status);
		}
	}

	/* If no CDR handled a dial begin message, make a new one */
	if (res && ast_strlen_zero(dial_status)) {
		struct cdr_object *new_cdr;

		new_cdr = cdr_object_create_and_append(cdr);
		if (!new_cdr) {
			return;
		}
		new_cdr->fn_table->process_dial_begin(new_cdr,
				party_a ? party_a->snapshot : NULL,
				party_b ? party_b->snapshot : NULL);
	}
	ao2_unlock(cdr);
}

static int cdr_object_finalize_party_b(void *obj, void *arg, int flags)
{
	struct cdr_object *cdr = obj;
	struct ast_channel_snapshot *party_b = arg;
	struct cdr_object *it_cdr;
	for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
		if (it_cdr->party_b.snapshot && !strcmp(it_cdr->party_b.snapshot->name, party_b->name)) {
			/* Don't transition to the finalized state - let the Party A do
			 * that when its ready
			 */
			cdr_object_finalize(it_cdr);
		}
	}
	return 0;
}

static int cdr_object_update_party_b(void *obj, void *arg, int flags)
{
	struct cdr_object *cdr = obj;
	struct ast_channel_snapshot *party_b = arg;
	struct cdr_object *it_cdr;
	for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
		if (!it_cdr->fn_table->process_party_b) {
			continue;
		}
		if (it_cdr->party_b.snapshot && !strcmp(it_cdr->party_b.snapshot->name, party_b->name)) {
			it_cdr->fn_table->process_party_b(it_cdr, party_b);
		}
	}
	return 0;
}

/*! \internal \brief Filter channel snapshots by technology */
static int filter_channel_snapshot(struct ast_channel_snapshot *snapshot)
{
	if (!strncmp(snapshot->name, "CBAnn", 5) ||
		!strncmp(snapshot->name, "CBRec", 5)) {
		return 1;
	}
	return 0;
}

/*! \internal \brief Filter a channel cache update */
static int filter_channel_cache_message(struct ast_channel_snapshot *old_snapshot,
		struct ast_channel_snapshot *new_snapshot)
{
	int ret = 0;

	/* Drop cache updates from certain channel technologies */
	if (old_snapshot) {
		ret |= filter_channel_snapshot(old_snapshot);
	}
	if (new_snapshot) {
		ret |= filter_channel_snapshot(new_snapshot);
	}

	return ret;
}

/*! \brief Determine if we need to add a new CDR based on snapshots */
static int check_new_cdr_needed(struct ast_channel_snapshot *old_snapshot,
		struct ast_channel_snapshot *new_snapshot)
{
	RAII_VAR(struct module_config *, mod_cfg,
			ao2_global_obj_ref(module_configs), ao2_cleanup);

	if (!new_snapshot) {
		return 0;
	}

	if (ast_test_flag(&new_snapshot->flags, AST_FLAG_ZOMBIE)) {
		return 0;
	}

	/* Auto-fall through will increment the priority but have no application */
	if (ast_strlen_zero(new_snapshot->appl)) {
		return 0;
	}

	if (old_snapshot && !strcmp(old_snapshot->context, new_snapshot->context)
			&& !strcmp(old_snapshot->exten, new_snapshot->exten)
			&& old_snapshot->priority == new_snapshot->priority
			&& !(strcmp(old_snapshot->appl, new_snapshot->appl))) {
		return 0;
	}

	return 1;
}

/*!
 * \brief Handler for Stasis-Core channel cache update messages
 * \param data Passed on
 * \param sub The stasis subscription for this message callback
 * \param topic The topic this message was published for
 * \param message The message
 */
static void handle_channel_cache_message(void *data, struct stasis_subscription *sub, struct stasis_topic *topic, struct stasis_message *message)
{
	RAII_VAR(struct cdr_object *, cdr, NULL, ao2_cleanup);
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);
	struct stasis_cache_update *update = stasis_message_data(message);
	struct ast_channel_snapshot *old_snapshot;
	struct ast_channel_snapshot *new_snapshot;
	const char *name;
	struct cdr_object *it_cdr;

	ast_assert(update != NULL);
	if (ast_channel_snapshot_type() != update->type) {
		return;
	}

	old_snapshot = stasis_message_data(update->old_snapshot);
	new_snapshot = stasis_message_data(update->new_snapshot);
	name = new_snapshot ? new_snapshot->name : old_snapshot->name;

	if (filter_channel_cache_message(old_snapshot, new_snapshot)) {
		return;
	}

	CDR_DEBUG(mod_cfg, "Channel Update message for %s: %u.%08u\n",
			name,
			(unsigned int)stasis_message_timestamp(message)->tv_sec,
			(unsigned int)stasis_message_timestamp(message)->tv_usec);

	if (new_snapshot && !old_snapshot) {
		cdr = cdr_object_alloc(new_snapshot);
		if (!cdr) {
			return;
		}
		ao2_link(active_cdrs_by_channel, cdr);
	}

	/* Handle Party A */
	if (!cdr) {
		cdr = ao2_find(active_cdrs_by_channel, name, OBJ_KEY);
	}
	if (!cdr) {
		ast_log(AST_LOG_WARNING, "No CDR for channel %s\n", name);
	} else {
		ao2_lock(cdr);
		if (new_snapshot) {
			int all_reject = 1;
			for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
				if (!it_cdr->fn_table->process_party_a) {
					continue;
				}
				CDR_DEBUG(mod_cfg, "%p - Processing new channel snapshot %s\n", it_cdr, new_snapshot->name);
				all_reject &= it_cdr->fn_table->process_party_a(it_cdr, new_snapshot);
			}
			if (all_reject && check_new_cdr_needed(old_snapshot, new_snapshot)) {
				/* We're not hung up and we have a new snapshot - we need a new CDR */
				struct cdr_object *new_cdr;
				new_cdr = cdr_object_create_and_append(cdr);
				new_cdr->fn_table->process_party_a(new_cdr, new_snapshot);
			}
		} else {
			CDR_DEBUG(mod_cfg, "%p - Beginning finalize/dispatch for %s\n", cdr, old_snapshot->name);
			for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
				cdr_object_finalize(it_cdr);
			}
			cdr_object_dispatch(cdr);
			ao2_unlink(active_cdrs_by_channel, cdr);
		}
		ao2_unlock(cdr);
	}

	/* Handle Party B */
	if (new_snapshot) {
		ao2_callback(active_cdrs_by_channel, OBJ_NODATA, cdr_object_update_party_b,
			new_snapshot);
	} else {
		ao2_callback(active_cdrs_by_channel, OBJ_NODATA, cdr_object_finalize_party_b,
			old_snapshot);
	}

}

struct bridge_leave_data {
	struct ast_bridge_snapshot *bridge;
	struct ast_channel_snapshot *channel;
};

/*! \brief Callback used to notify CDRs of a Party B leaving the bridge */
static int cdr_object_party_b_left_bridge_cb(void *obj, void *arg, int flags)
{
	struct cdr_object *cdr = obj;
	struct bridge_leave_data *leave_data = arg;
	struct cdr_object *it_cdr;

	if (strcmp(cdr->bridge, leave_data->bridge->uniqueid)) {
		return 0;
	}
	for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
		if (it_cdr->fn_table != &bridge_state_fn_table) {
			continue;
		}
		if (!it_cdr->party_b.snapshot) {
			continue;
		}
		if (strcmp(it_cdr->party_b.snapshot->name, leave_data->channel->name)) {
			continue;
		}
		if (!it_cdr->fn_table->process_bridge_leave(it_cdr, leave_data->bridge, leave_data->channel)) {
			/* Update the end times for this CDR. We don't want to actually
			 * finalize it, as the Party A will eventually need to leave, which
			 * will switch the records to pending bridged.
			 */
			cdr_object_finalize(it_cdr);
		}
	}
	return 0;
}

/*! \brief Filter bridge messages based on bridge technology */
static int filter_bridge_messages(struct ast_bridge_snapshot *bridge)
{
	/* Ignore holding bridge technology messages. We treat this simply as an application
	 * that a channel enters into.
	 */
	if (!strcmp(bridge->technology, "holding_bridge")) {
		return 1;
	}
	return 0;
}

/*!
 * \brief Handler for when a channel leaves a bridge
 * \param bridge The \ref ast_bridge_snapshot representing the bridge
 * \param channel The \ref ast_channel_snapshot representing the channel
 */
static void handle_bridge_leave_message(void *data, struct stasis_subscription *sub,
		struct stasis_topic *topic, struct stasis_message *message)
{
	struct ast_bridge_blob *update = stasis_message_data(message);
	struct ast_bridge_snapshot *bridge = update->bridge;
	struct ast_channel_snapshot *channel = update->channel;
	RAII_VAR(struct module_config *, mod_cfg,
			ao2_global_obj_ref(module_configs), ao2_cleanup);
	RAII_VAR(struct cdr_object *, cdr,
			ao2_find(active_cdrs_by_channel, channel->name, OBJ_KEY),
			ao2_cleanup);
	struct cdr_object *it_cdr;
	struct cdr_object *pending_cdr;
	struct bridge_leave_data leave_data = {
		.bridge = bridge,
		.channel = channel,
	};
	int left_bridge = 0;

	if (filter_bridge_messages(bridge)) {
		return;
	}

	CDR_DEBUG(mod_cfg, "Bridge Leave message: %u.%08u\n", (unsigned int)stasis_message_timestamp(message)->tv_sec, (unsigned int)stasis_message_timestamp(message)->tv_usec);

	if (!cdr) {
		ast_log(AST_LOG_WARNING, "No CDR for channel %s\n", channel->name);
		return;
	}

	/* Party A */
	ao2_lock(cdr);
	for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
		if (!it_cdr->fn_table->process_bridge_leave) {
			continue;
		}
		CDR_DEBUG(mod_cfg, "%p - Processing Bridge Leave for %s\n",
				it_cdr, channel->name);
		if (!it_cdr->fn_table->process_bridge_leave(it_cdr, bridge, channel)) {
			ast_string_field_set(it_cdr, bridge, "");
			left_bridge = 1;
		}
	}
	if (!left_bridge) {
		ao2_unlock(cdr);
		return;
	}

	ao2_unlink(active_cdrs_by_bridge, cdr);

	/* Create a new pending record. If the channel decides to do something else,
	 * the pending record will handle it - otherwise, if gets dropped.
	 */
	pending_cdr = cdr_object_create_and_append(cdr);
	cdr_object_transition_state(pending_cdr, &bridged_pending_state_fn_table);
	ao2_unlock(cdr);

	/* Party B */
	ao2_callback(active_cdrs_by_bridge, OBJ_NODATA,
			cdr_object_party_b_left_bridge_cb,
			&leave_data);
}

struct bridge_candidate {
	struct cdr_object *cdr;					/*!< The actual CDR this candidate belongs to, either as A or B */
	struct cdr_object_snapshot candidate;	/*!< The candidate for a new pairing */
};

/*! \internal
 * \brief Comparison function for \ref bridge_candidate objects
 */
static int bridge_candidate_cmp_fn(void *obj, void *arg, int flags)
{
	struct bridge_candidate *left = obj;
	struct bridge_candidate *right = arg;
	const char *match = (flags & OBJ_KEY) ? arg : right->candidate.snapshot->name;
	return strcasecmp(left->candidate.snapshot->name, match) ? 0 : (CMP_MATCH | CMP_STOP);
}

/*! \internal
 * \brief Hash function for \ref bridge_candidate objects
 */
static int bridge_candidate_hash_fn(const void *obj, const int flags)
{
	const struct bridge_candidate *bc = obj;
	const char *id = (flags & OBJ_KEY) ? obj : bc->candidate.snapshot->name;
	return ast_str_case_hash(id);
}

/*! \brief \ref bridge_candidate Destructor */
static void bridge_candidate_dtor(void *obj)
{
	struct bridge_candidate *bcand = obj;
	ao2_cleanup(bcand->cdr);
	ao2_cleanup(bcand->candidate.snapshot);
	free_variables(&bcand->candidate.variables);
}

/*!
 * \brief \ref bridge_candidate Constructor
 * \param cdr The \ref cdr_object that is a candidate for being compared to in
 *  a bridge operation
 * \param candidate The \ref cdr_object_snapshot candidate snapshot in the CDR
 *  that should be used during the operaton
 */
static struct bridge_candidate *bridge_candidate_alloc(struct cdr_object *cdr, struct cdr_object_snapshot *candidate)
{
	struct bridge_candidate *bcand;

	bcand = ao2_alloc(sizeof(*bcand), bridge_candidate_dtor);
	if (!bcand) {
		return NULL;
	}
	bcand->cdr = cdr;
	ao2_ref(bcand->cdr, +1);
	bcand->candidate.flags = candidate->flags;
	strcpy(bcand->candidate.userfield, candidate->userfield);
	bcand->candidate.snapshot = candidate->snapshot;
	ao2_ref(bcand->candidate.snapshot, +1);
	copy_variables(&bcand->candidate.variables, &candidate->variables);

	return bcand;
}

/*!
 * \internal \brief Build and add bridge candidates based on a CDR
 * \param bridge_id The ID of the bridge we need candidates for
 * \param candidates The container of \ref bridge_candidate objects
 * \param cdr The \ref cdr_object that is our candidate
 * \param party_a Non-zero if we should look at the Party A channel; 0 if Party B
 */
static void add_candidate_for_bridge(const char *bridge_id,
		struct ao2_container *candidates,
		struct cdr_object *cdr,
		int party_a)
{
	struct cdr_object *it_cdr;

	for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
		struct cdr_object_snapshot *party_snapshot;
		RAII_VAR(struct bridge_candidate *, bcand, NULL, ao2_cleanup);

		party_snapshot = party_a ? &it_cdr->party_a : &it_cdr->party_b;

		if (it_cdr->fn_table != &bridge_state_fn_table || strcmp(bridge_id, it_cdr->bridge)) {
			continue;
		}

		if (!party_snapshot->snapshot) {
			continue;
		}

		/* Don't add a party twice */
		bcand = ao2_find(candidates, party_snapshot->snapshot->name, OBJ_KEY);
		if (bcand) {
			continue;
		}

		bcand = bridge_candidate_alloc(it_cdr, party_snapshot);
		if (bcand) {
			ao2_link(candidates, bcand);
		}
	}
}

/*!
 * \brief Create new \ref bridge_candidate objects for each party currently
 * in a bridge
 * \param bridge The \param ast_bridge_snapshot for the bridge we're processing
 *
 * Note that we use two passes here instead of one so that we only create a
 * candidate for a party B if they are never a party A in the bridge. Otherwise,
 * we don't care about them.
 */
static struct ao2_container *create_candidates_for_bridge(struct ast_bridge_snapshot *bridge)
{
	struct ao2_container *candidates = ao2_container_alloc(51, bridge_candidate_hash_fn, bridge_candidate_cmp_fn);
	char *bridge_id = ast_strdupa(bridge->uniqueid);
	struct ao2_iterator *it_cdrs;
	struct cdr_object *cand_cdr_master;

	if (!candidates) {
		return NULL;
	}

	/* For each CDR that has a record in the bridge, get their Party A and
	 * make them a candidate. Note that we do this in two passes as opposed to one so
	 * that we give preference CDRs where the channel is Party A */
	it_cdrs = ao2_callback(active_cdrs_by_bridge, OBJ_MULTIPLE | OBJ_KEY,
			cdr_object_bridge_cmp_fn, bridge_id);
	if (!it_cdrs) {
		/* No one in the bridge yet! */
		ao2_cleanup(candidates);
		return NULL;
	}
	while ((cand_cdr_master = ao2_iterator_next(it_cdrs))) {
		SCOPED_AO2LOCK(lock, cand_cdr_master);
		add_candidate_for_bridge(bridge->uniqueid, candidates, cand_cdr_master, 1);
	}
	ao2_iterator_destroy(it_cdrs);

	/* For each CDR that has a record in the bridge, get their Party B and
	 * make them a candidate. */
	it_cdrs = ao2_callback(active_cdrs_by_bridge, OBJ_MULTIPLE | OBJ_KEY,
			cdr_object_bridge_cmp_fn, bridge_id);
	if (!it_cdrs) {
		/* Now it's just an error. */
		ao2_cleanup(candidates);
		return NULL;
	}
	while ((cand_cdr_master = ao2_iterator_next(it_cdrs))) {
		SCOPED_AO2LOCK(lock, cand_cdr_master);
		add_candidate_for_bridge(bridge->uniqueid, candidates, cand_cdr_master, 0);
	}
	ao2_iterator_destroy(it_cdrs);

	return candidates;
}

/*!
 * \internal \brief Create a new CDR, append it to an existing CDR, and update its snapshots
 * \note The new CDR will be automatically transitioned to the bridge state
 */
static void bridge_candidate_add_to_cdr(struct cdr_object *cdr,
		const char *bridge_id,
		struct cdr_object_snapshot *party_b)
{
	struct cdr_object *new_cdr;

	new_cdr = cdr_object_create_and_append(cdr);
	cdr_object_snapshot_copy(&new_cdr->party_b, party_b);
	cdr_object_check_party_a_answer(new_cdr);
	ast_string_field_set(new_cdr, bridge, cdr->bridge);
	cdr_object_transition_state(new_cdr, &bridge_state_fn_table);
}

/*!
 * \brief Process a single \ref bridge_candidate. Note that this is called as
 * part of an \ref ao2_callback on an \ref ao2_container of \ref bridge_candidate
 * objects previously created by \ref create_candidates_for_bridge.
 *
 * \param obj The \ref bridge_candidate being processed
 * \param arg The \ref cdr_object that is being compared against the candidates
 *
 * The purpose of this function is to create the necessary CDR entries as a
 * result of \ref cdr_object having entered the same bridge as the CDR
 * represented by \ref bridge_candidate.
 */
static int bridge_candidate_process(void *obj, void *arg, int flags)
{
	struct bridge_candidate *bcand = obj;
	struct cdr_object *cdr = arg;
	struct cdr_object_snapshot *party_a;

	/* If the candidate is us or someone we've taken on, pass on by */
	if (!strcmp(cdr->party_a.snapshot->name, bcand->candidate.snapshot->name)
		|| (cdr->party_b.snapshot && !(strcmp(cdr->party_b.snapshot->name, bcand->candidate.snapshot->name)))) {
		return 0;
	}

	party_a = cdr_object_pick_party_a(&cdr->party_a, &bcand->candidate);
	/* We're party A - make a new CDR, append it to us, and set the candidate as
	 * Party B */
	if (!strcmp(party_a->snapshot->name, cdr->party_a.snapshot->name)) {
		bridge_candidate_add_to_cdr(cdr, cdr->bridge, &bcand->candidate);
		return 0;
	}

	/* We're Party B. Check if the candidate is the CDR's Party A. If so, find out if we
	 * can add ourselves directly as the Party B, or if we need a new CDR. */
	if (!strcmp(bcand->cdr->party_a.snapshot->name, bcand->candidate.snapshot->name)) {
		if (bcand->cdr->party_b.snapshot
				&& strcmp(bcand->cdr->party_b.snapshot->name, cdr->party_a.snapshot->name)) {
			bridge_candidate_add_to_cdr(bcand->cdr, cdr->bridge, &cdr->party_a);
		} else {
			cdr_object_snapshot_copy(&bcand->cdr->party_b, &cdr->party_a);
			/* It's possible that this joined at one point and was never chosen
			 * as party A. Clear their end time, as it would be set in such a
			 * case.
			 */
			memset(&bcand->cdr->end, 0, sizeof(bcand->cdr->end));
		}
	} else {
		/* We are Party B to a candidate CDR's Party B. Since a candidate
		 * CDR will only have a Party B represented here if that channel
		 * was never a Party A in the bridge, we have to go looking for
		 * that channel's primary CDR record.
		 */
		struct cdr_object *b_party = ao2_find(active_cdrs_by_channel, bcand->candidate.snapshot->name, OBJ_KEY);
		if (!b_party) {
			/* Holy cow - no CDR? */
			b_party = cdr_object_alloc(bcand->candidate.snapshot);
			cdr_object_snapshot_copy(&b_party->party_a, &bcand->candidate);
			cdr_object_snapshot_copy(&b_party->party_b, &cdr->party_a);
			cdr_object_check_party_a_answer(b_party);
			ast_string_field_set(b_party, bridge, cdr->bridge);
			cdr_object_transition_state(b_party, &bridge_state_fn_table);
			ao2_link(active_cdrs_by_channel, b_party);
		} else {
			bridge_candidate_add_to_cdr(b_party, cdr->bridge, &cdr->party_a);
		}
		ao2_link(active_cdrs_by_bridge, b_party);
		ao2_ref(b_party, -1);
	}

	return 0;
}

/*!
 * \brief Handle creating bridge pairings for the \ref cdr_object that just
 * entered a bridge
 * \param cdr The \ref cdr_object that just entered the bridge
 * \param bridge The \ref ast_bridge_snapshot representing the bridge it just entered
 */
static void handle_bridge_pairings(struct cdr_object *cdr, struct ast_bridge_snapshot *bridge)
{
	RAII_VAR(struct ao2_container *, candidates,
			create_candidates_for_bridge(bridge),
			ao2_cleanup);

	if (!candidates) {
		return;
	}

	ao2_callback(candidates, OBJ_NODATA,
			bridge_candidate_process,
			cdr);

	return;
}

/*!
 * \brief Handler for Stasis-Core bridge enter messages
 * \param data Passed on
 * \param sub The stasis subscription for this message callback
 * \param topic The topic this message was published for
 * \param message The message - hopefully a bridge one!
 */
static void handle_bridge_enter_message(void *data, struct stasis_subscription *sub,
		struct stasis_topic *topic, struct stasis_message *message)
{
	struct ast_bridge_blob *update = stasis_message_data(message);
	struct ast_bridge_snapshot *bridge = update->bridge;
	struct ast_channel_snapshot *channel = update->channel;
	RAII_VAR(struct cdr_object *, cdr,
			ao2_find(active_cdrs_by_channel, channel->name, OBJ_KEY),
			ao2_cleanup);
	RAII_VAR(struct module_config *, mod_cfg,
			ao2_global_obj_ref(module_configs), ao2_cleanup);
	int res = 1;
	struct cdr_object *it_cdr;
	struct cdr_object *handled_cdr = NULL;

	if (filter_bridge_messages(bridge)) {
		return;
	}

	CDR_DEBUG(mod_cfg, "Bridge Enter message: %u.%08u\n", (unsigned int)stasis_message_timestamp(message)->tv_sec, (unsigned int)stasis_message_timestamp(message)->tv_usec);

	if (!cdr) {
		ast_log(AST_LOG_WARNING, "No CDR for channel %s\n", channel->name);
		return;
	}

	ao2_lock(cdr);

	for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
		if (it_cdr->fn_table->process_party_a) {
			CDR_DEBUG(mod_cfg, "%p - Updating Party A %s snapshot\n", it_cdr,
					channel->name);
			it_cdr->fn_table->process_party_a(it_cdr, channel);
		}

		/* Notify all states that they have entered a bridge */
		if (it_cdr->fn_table->process_bridge_enter) {
			CDR_DEBUG(mod_cfg, "%p - Processing bridge enter for %s\n", it_cdr,
					channel->name);
			res &= it_cdr->fn_table->process_bridge_enter(it_cdr, bridge, channel);
			if (!res && !handled_cdr) {
				handled_cdr = it_cdr;
			}
		}
	}

	if (res) {
		/* We didn't win on any - end this CDR. If someone else comes in later
		 * that is Party B to this CDR, it can re-activate this CDR.
		 */
		cdr_object_finalize(cdr);
	}

	/* Create the new matchings, but only for either:
	 *  * The first CDR in the chain that handled it. This avoids issues with
	 *    forked CDRs.
	 *  * If no one handled it, the last CDR in the chain. This would occur if
	 *    a CDR joined a bridge and it wasn't Party A for anyone. We still need
	 *    to make pairings with everyone in the bridge.
	 */
	if (!handled_cdr) {
		handled_cdr = cdr->last;
	}
	handle_bridge_pairings(handled_cdr, bridge);

	ao2_link(active_cdrs_by_bridge, cdr);
	ao2_unlock(cdr);
}

struct ast_cdr_config *ast_cdr_get_config(void)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);
	ao2_ref(mod_cfg->general, +1);
	return mod_cfg->general;
}

void ast_cdr_set_config(struct ast_cdr_config *config)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);
	ao2_cleanup(mod_cfg->general);
	mod_cfg->general = config;
	ao2_ref(mod_cfg->general, +1);
}

int ast_cdr_is_enabled(void)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);
	return ast_test_flag(&mod_cfg->general->settings, CDR_ENABLED);
}

int ast_cdr_register(const char *name, const char *desc, ast_cdrbe be)
{
	struct cdr_beitem *i = NULL;

	if (!name)
		return -1;

	if (!be) {
		ast_log(LOG_WARNING, "CDR engine '%s' lacks backend\n", name);
		return -1;
	}

	AST_RWLIST_WRLOCK(&be_list);
	AST_RWLIST_TRAVERSE(&be_list, i, list) {
		if (!strcasecmp(name, i->name)) {
			ast_log(LOG_WARNING, "Already have a CDR backend called '%s'\n", name);
			AST_RWLIST_UNLOCK(&be_list);
			return -1;
		}
	}

	if (!(i = ast_calloc(1, sizeof(*i))))
		return -1;

	i->be = be;
	ast_copy_string(i->name, name, sizeof(i->name));
	ast_copy_string(i->desc, desc, sizeof(i->desc));

	AST_RWLIST_INSERT_HEAD(&be_list, i, list);
	AST_RWLIST_UNLOCK(&be_list);

	return 0;
}

void ast_cdr_unregister(const char *name)
{
	struct cdr_beitem *i = NULL;

	AST_RWLIST_WRLOCK(&be_list);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&be_list, i, list) {
		if (!strcasecmp(name, i->name)) {
			AST_RWLIST_REMOVE_CURRENT(list);
			break;
		}
	}
	AST_RWLIST_TRAVERSE_SAFE_END;
	AST_RWLIST_UNLOCK(&be_list);

	if (i) {
		ast_verb(2, "Unregistered '%s' CDR backend\n", name);
		ast_free(i);
	}
}

struct ast_cdr *ast_cdr_dup(struct ast_cdr *cdr)
{
	struct ast_cdr *newcdr;

	if (!cdr) {
		return NULL;
	}
	newcdr = ast_cdr_alloc();
	if (!newcdr) {
		return NULL;
	}

	memcpy(newcdr, cdr, sizeof(*newcdr));
	memset(&newcdr->varshead, 0, sizeof(newcdr->varshead));
	copy_variables(&newcdr->varshead, &cdr->varshead);
	newcdr->next = NULL;

	return newcdr;
}

static const char *cdr_format_var_internal(struct ast_cdr *cdr, const char *name)
{
	struct ast_var_t *variables;
	struct varshead *headp = &cdr->varshead;

	if (ast_strlen_zero(name)) {
		return NULL;
	}

	AST_LIST_TRAVERSE(headp, variables, entries) {
		if (!strcasecmp(name, ast_var_name(variables))) {
			return ast_var_value(variables);
		}
	}

	return '\0';
}

static void cdr_get_tv(struct timeval when, const char *fmt, char *buf, int bufsize)
{
	if (fmt == NULL) {	/* raw mode */
		snprintf(buf, bufsize, "%ld.%06ld", (long)when.tv_sec, (long)when.tv_usec);
	} else {
		if (when.tv_sec) {
			struct ast_tm tm;

			ast_localtime(&when, &tm, NULL);
			ast_strftime(buf, bufsize, fmt, &tm);
		}
	}
}

void ast_cdr_format_var(struct ast_cdr *cdr, const char *name, char **ret, char *workspace, int workspacelen, int raw)
{
	const char *fmt = "%Y-%m-%d %T";
	const char *varbuf;

	if (!cdr) {
		return;
	}

	*ret = NULL;

	if (!strcasecmp(name, "clid")) {
		ast_copy_string(workspace, cdr->clid, workspacelen);
	} else if (!strcasecmp(name, "src")) {
		ast_copy_string(workspace, cdr->src, workspacelen);
	} else if (!strcasecmp(name, "dst")) {
		ast_copy_string(workspace, cdr->dst, workspacelen);
	} else if (!strcasecmp(name, "dcontext")) {
		ast_copy_string(workspace, cdr->dcontext, workspacelen);
	} else if (!strcasecmp(name, "channel")) {
		ast_copy_string(workspace, cdr->channel, workspacelen);
	} else if (!strcasecmp(name, "dstchannel")) {
		ast_copy_string(workspace, cdr->dstchannel, workspacelen);
	} else if (!strcasecmp(name, "lastapp")) {
		ast_copy_string(workspace, cdr->lastapp, workspacelen);
	} else if (!strcasecmp(name, "lastdata")) {
		ast_copy_string(workspace, cdr->lastdata, workspacelen);
	} else if (!strcasecmp(name, "start")) {
		cdr_get_tv(cdr->start, raw ? NULL : fmt, workspace, workspacelen);
	} else if (!strcasecmp(name, "answer")) {
		cdr_get_tv(cdr->answer, raw ? NULL : fmt, workspace, workspacelen);
	} else if (!strcasecmp(name, "end")) {
		cdr_get_tv(cdr->end, raw ? NULL : fmt, workspace, workspacelen);
	} else if (!strcasecmp(name, "duration")) {
		snprintf(workspace, workspacelen, "%ld", cdr->end.tv_sec != 0 ? cdr->duration : (long)ast_tvdiff_ms(ast_tvnow(), cdr->start) / 1000);
	} else if (!strcasecmp(name, "billsec")) {
		snprintf(workspace, workspacelen, "%ld", (cdr->billsec || !ast_tvzero(cdr->end) || ast_tvzero(cdr->answer)) ? cdr->billsec : (long)ast_tvdiff_ms(ast_tvnow(), cdr->answer) / 1000);
	} else if (!strcasecmp(name, "disposition")) {
		if (raw) {
			snprintf(workspace, workspacelen, "%ld", cdr->disposition);
		} else {
			ast_copy_string(workspace, ast_cdr_disp2str(cdr->disposition), workspacelen);
		}
	} else if (!strcasecmp(name, "amaflags")) {
		if (raw) {
			snprintf(workspace, workspacelen, "%ld", cdr->amaflags);
		} else {
			ast_copy_string(workspace, ast_channel_amaflags2string(cdr->amaflags), workspacelen);
		}
	} else if (!strcasecmp(name, "accountcode")) {
		ast_copy_string(workspace, cdr->accountcode, workspacelen);
	} else if (!strcasecmp(name, "peeraccount")) {
		ast_copy_string(workspace, cdr->peeraccount, workspacelen);
	} else if (!strcasecmp(name, "uniqueid")) {
		ast_copy_string(workspace, cdr->uniqueid, workspacelen);
	} else if (!strcasecmp(name, "linkedid")) {
		ast_copy_string(workspace, cdr->linkedid, workspacelen);
	} else if (!strcasecmp(name, "userfield")) {
		ast_copy_string(workspace, cdr->userfield, workspacelen);
	} else if (!strcasecmp(name, "sequence")) {
		snprintf(workspace, workspacelen, "%d", cdr->sequence);
	} else if ((varbuf = cdr_format_var_internal(cdr, name))) {
		ast_copy_string(workspace, varbuf, workspacelen);
	} else {
		workspace[0] = '\0';
	}

	if (!ast_strlen_zero(workspace)) {
		*ret = workspace;
	}
}

/*
 * \internal
 * \brief Callback that finds all CDRs that reference a particular channel
 */
static int cdr_object_select_all_by_channel_cb(void *obj, void *arg, int flags)
{
	struct cdr_object *cdr = obj;
	const char *name = arg;
	if (!(flags & OBJ_KEY)) {
		return 0;
	}
	if (!strcasecmp(cdr->party_a.snapshot->name, name) ||
			(cdr->party_b.snapshot && !strcasecmp(cdr->party_b.snapshot->name, name))) {
		return CMP_MATCH;
	}
	return 0;
}

/* Read Only CDR variables */
static const char * const cdr_readonly_vars[] = { "clid", "src", "dst", "dcontext", "channel", "dstchannel",
						  "lastapp", "lastdata", "start", "answer", "end", "duration",
						  "billsec", "disposition", "amaflags", "accountcode", "uniqueid", "linkedid",
						  "userfield", "sequence", NULL };

int ast_cdr_setvar(const char *channel_name, const char *name, const char *value)
{
	struct cdr_object *cdr;
	struct cdr_object *it_cdr;
	struct ao2_iterator *it_cdrs;
	char *arg = ast_strdupa(channel_name);
	int x;

	for (x = 0; cdr_readonly_vars[x]; x++) {
		if (!strcasecmp(name, cdr_readonly_vars[x])) {
			ast_log(LOG_ERROR, "Attempt to set the '%s' read-only variable!\n", name);
			return -1;
		}
	}

	it_cdrs = ao2_callback(active_cdrs_by_channel, OBJ_MULTIPLE | OBJ_KEY, cdr_object_select_all_by_channel_cb, arg);
	if (!it_cdrs) {
		ast_log(AST_LOG_ERROR, "Unable to find CDR for channel %s\n", channel_name);
		return -1;
	}

	while ((cdr = ao2_iterator_next(it_cdrs))) {
		ao2_lock(cdr);
		for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
			struct varshead *headp = NULL;
			if (it_cdr->fn_table == &finalized_state_fn_table) {
				continue;
			}
			if (!strcmp(channel_name, it_cdr->party_a.snapshot->name)) {
				headp = &it_cdr->party_a.variables;
			} else if (it_cdr->party_b.snapshot && !strcmp(channel_name, it_cdr->party_b.snapshot->name)) {
				headp = &it_cdr->party_b.variables;
			}
			if (headp) {
				set_variable(headp, name, value);
			}
		}
		ao2_unlock(cdr);
		ao2_ref(cdr, -1);
	}
	ao2_iterator_destroy(it_cdrs);

	return 0;
}

/*!
 * \brief Format a variable on a \ref cdr_object
 */
static void cdr_object_format_var_internal(struct cdr_object *cdr, const char *name, char *value, size_t length)
{
	struct ast_var_t *variable;

	AST_LIST_TRAVERSE(&cdr->party_a.variables, variable, entries) {
		if (!strcasecmp(name, ast_var_name(variable))) {
			ast_copy_string(value, ast_var_value(variable), length);
			return;
		}
	}

	*value = '\0';
}

/*!
 * \brief Format one of the standard properties on a \ref cdr_object
 */
static int cdr_object_format_property(struct cdr_object *cdr_obj, const char *name, char *value, size_t length)
{
	struct ast_channel_snapshot *party_a = cdr_obj->party_a.snapshot;
	struct ast_channel_snapshot *party_b = cdr_obj->party_b.snapshot;

	if (!strcasecmp(name, "clid")) {
		ast_callerid_merge(value, length, party_a->caller_name, party_a->caller_number, "");
	} else if (!strcasecmp(name, "src")) {
		ast_copy_string(value, party_a->caller_number, length);
	} else if (!strcasecmp(name, "dst")) {
		ast_copy_string(value, party_a->exten, length);
	} else if (!strcasecmp(name, "dcontext")) {
		ast_copy_string(value, party_a->context, length);
	} else if (!strcasecmp(name, "channel")) {
		ast_copy_string(value, party_a->name, length);
	} else if (!strcasecmp(name, "dstchannel")) {
		if (party_b) {
			ast_copy_string(value, party_b->name, length);
		} else {
			ast_copy_string(value, "", length);
		}
	} else if (!strcasecmp(name, "lastapp")) {
		ast_copy_string(value, party_a->appl, length);
	} else if (!strcasecmp(name, "lastdata")) {
		ast_copy_string(value, party_a->data, length);
	} else if (!strcasecmp(name, "start")) {
		cdr_get_tv(cdr_obj->start, NULL, value, length);
	} else if (!strcasecmp(name, "answer")) {
		cdr_get_tv(cdr_obj->answer, NULL, value, length);
	} else if (!strcasecmp(name, "end")) {
		cdr_get_tv(cdr_obj->end, NULL, value, length);
	} else if (!strcasecmp(name, "duration")) {
		snprintf(value, length, "%ld", cdr_object_get_duration(cdr_obj));
	} else if (!strcasecmp(name, "billsec")) {
		snprintf(value, length, "%ld", cdr_object_get_billsec(cdr_obj));
	} else if (!strcasecmp(name, "disposition")) {
		snprintf(value, length, "%d", cdr_obj->disposition);
	} else if (!strcasecmp(name, "amaflags")) {
		snprintf(value, length, "%d", party_a->amaflags);
	} else if (!strcasecmp(name, "accountcode")) {
		ast_copy_string(value, party_a->accountcode, length);
	} else if (!strcasecmp(name, "peeraccount")) {
		if (party_b) {
			ast_copy_string(value, party_b->accountcode, length);
		} else {
			ast_copy_string(value, "", length);
		}
	} else if (!strcasecmp(name, "uniqueid")) {
		ast_copy_string(value, party_a->uniqueid, length);
	} else if (!strcasecmp(name, "linkedid")) {
		ast_copy_string(value, cdr_obj->linkedid, length);
	} else if (!strcasecmp(name, "userfield")) {
		ast_copy_string(value, cdr_obj->party_a.userfield, length);
	} else if (!strcasecmp(name, "sequence")) {
		snprintf(value, length, "%d", cdr_obj->sequence);
	} else {
		return 1;
	}

	return 0;
}

int ast_cdr_getvar(const char *channel_name, const char *name, char *value, size_t length)
{
	RAII_VAR(struct cdr_object *, cdr,
		ao2_find(active_cdrs_by_channel, channel_name, OBJ_KEY),
		ao2_cleanup);
	struct cdr_object *cdr_obj;

	if (!cdr) {
		ast_log(AST_LOG_ERROR, "Unable to find CDR for channel %s\n", channel_name);
		return 1;
	}

	if (ast_strlen_zero(name)) {
		return 1;
	}

	ao2_lock(cdr);

	cdr_obj = cdr->last;

	if (cdr_object_format_property(cdr_obj, name, value, length)) {
		/* Property failed; attempt variable */
		cdr_object_format_var_internal(cdr_obj, name, value, length);
	}
	ao2_unlock(cdr);

	return 0;
}

int ast_cdr_serialize_variables(const char *channel_name, struct ast_str **buf, char delim, char sep)
{
	RAII_VAR(struct cdr_object *, cdr,
		ao2_find(active_cdrs_by_channel, channel_name, OBJ_KEY),
		ao2_cleanup);
	struct cdr_object *it_cdr;
	struct ast_var_t *variable;
	const char *var;
	RAII_VAR(char *, workspace, ast_malloc(256), ast_free);
	int total = 0, x = 0, i;

	if (!workspace) {
		return 1;
	}

	if (!cdr) {
		ast_log(AST_LOG_ERROR, "Unable to find CDR for channel %s\n", channel_name);
		return 1;
	}

	ast_str_reset(*buf);

	ao2_lock(cdr);
	for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
		if (++x > 1)
			ast_str_append(buf, 0, "\n");

		AST_LIST_TRAVERSE(&it_cdr->party_a.variables, variable, entries) {
			if (!(var = ast_var_name(variable))) {
				continue;
			}

			if (ast_str_append(buf, 0, "level %d: %s%c%s%c", x, var, delim, S_OR(ast_var_value(variable), ""), sep) < 0) {
				ast_log(LOG_ERROR, "Data Buffer Size Exceeded!\n");
				break;
			}

			total++;
		}

		for (i = 0; cdr_readonly_vars[i]; i++) {
			/* null out the workspace, because the cdr_get_tv() won't write anything if time is NULL, so you get old vals */
			workspace[0] = 0;
			cdr_object_format_property(it_cdr, cdr_readonly_vars[i], workspace, sizeof(workspace));

			if (!ast_strlen_zero(workspace)
				&& ast_str_append(buf, 0, "level %d: %s%c%s%c", x, cdr_readonly_vars[i], delim, workspace, sep) < 0) {
				ast_log(LOG_ERROR, "Data Buffer Size Exceeded!\n");
				break;
			}
			total++;
		}
	}

	return total;
}

void ast_cdr_free(struct ast_cdr *cdr)
{
	while (cdr) {
		struct ast_cdr *next = cdr->next;

		free_variables(&cdr->varshead);
		ast_free(cdr);
		cdr = next;
	}
}

struct ast_cdr *ast_cdr_alloc(void)
{
	struct ast_cdr *x;

	x = ast_calloc(1, sizeof(*x));
	return x;
}

const char *ast_cdr_disp2str(int disposition)
{
	switch (disposition) {
	case AST_CDR_NULL:
		return "NO ANSWER"; /* by default, for backward compatibility */
	case AST_CDR_NOANSWER:
		return "NO ANSWER";
	case AST_CDR_FAILED:
		return "FAILED";
	case AST_CDR_BUSY:
		return "BUSY";
	case AST_CDR_ANSWERED:
		return "ANSWERED";
	case AST_CDR_CONGESTION:
		return "CONGESTION";
	}
	return "UNKNOWN";
}

struct party_b_userfield_update {
	const char *channel_name;
	const char *userfield;
};

/*! \brief Callback used to update the userfield on Party B on all CDRs */
static int cdr_object_update_party_b_userfield_cb(void *obj, void *arg, int flags)
{
	struct cdr_object *cdr = obj;
	struct party_b_userfield_update *info = arg;
	struct cdr_object *it_cdr;
	for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
		if (it_cdr->fn_table == &finalized_state_fn_table) {
			continue;
		}
		if (it_cdr->party_b.snapshot
			&& !strcmp(it_cdr->party_b.snapshot->name, info->channel_name)) {
			strcpy(it_cdr->party_b.userfield, info->userfield);
		}
	}
	return 0;
}

void ast_cdr_setuserfield(const char *channel_name, const char *userfield)
{
	RAII_VAR(struct cdr_object *, cdr,
			ao2_find(active_cdrs_by_channel, channel_name, OBJ_KEY),
			ao2_cleanup);
	struct party_b_userfield_update party_b_info = {
			.channel_name = channel_name,
			.userfield = userfield,
	};
	struct cdr_object *it_cdr;

	/* Handle Party A */
	if (cdr) {
		ao2_lock(cdr);
		for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
			if (it_cdr->fn_table == &finalized_state_fn_table) {
				continue;
			}
			strcpy(it_cdr->party_a.userfield, userfield);
		}
		ao2_unlock(cdr);
	}

	/* Handle Party B */
	ao2_callback(active_cdrs_by_channel, OBJ_NODATA,
			cdr_object_update_party_b_userfield_cb,
			&party_b_info);

}

static void post_cdr(struct ast_cdr *cdr)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);
	struct cdr_beitem *i;

	for (; cdr ; cdr = cdr->next) {
		/* For people, who don't want to see unanswered single-channel events */
		if (!ast_test_flag(&mod_cfg->general->settings, CDR_UNANSWERED) &&
				cdr->disposition < AST_CDR_ANSWERED &&
				(ast_strlen_zero(cdr->channel) || ast_strlen_zero(cdr->dstchannel))) {
			continue;
		}

		if (ast_test_flag(cdr, AST_CDR_FLAG_DISABLE)) {
			continue;
		}
		AST_RWLIST_RDLOCK(&be_list);
		AST_RWLIST_TRAVERSE(&be_list, i, list) {
			i->be(cdr);
		}
		AST_RWLIST_UNLOCK(&be_list);
	}
}

int ast_cdr_set_property(const char *channel_name, enum ast_cdr_options option)
{
	RAII_VAR(struct cdr_object *, cdr,
			ao2_find(active_cdrs_by_channel, channel_name, OBJ_KEY),
			ao2_cleanup);
	struct cdr_object *it_cdr;

	if (!cdr) {
		return -1;
	}

	ao2_lock(cdr);
	for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
		if (it_cdr->fn_table == &finalized_state_fn_table) {
			continue;
		}
		ast_set_flag(&it_cdr->flags, option);
	}
	ao2_unlock(cdr);

	return 0;
}

int ast_cdr_clear_property(const char *channel_name, enum ast_cdr_options option)
{
	RAII_VAR(struct cdr_object *, cdr,
			ao2_find(active_cdrs_by_channel, channel_name, OBJ_KEY),
			ao2_cleanup);
	struct cdr_object *it_cdr;

	if (!cdr) {
		return -1;
	}

	ao2_lock(cdr);
	for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
		if (it_cdr->fn_table == &finalized_state_fn_table) {
			continue;
		}
		ast_clear_flag(&it_cdr->flags, option);
	}
	ao2_unlock(cdr);

	return 0;
}

int ast_cdr_reset(const char *channel_name, struct ast_flags *options)
{
	RAII_VAR(struct cdr_object *, cdr,
			ao2_find(active_cdrs_by_channel, channel_name, OBJ_KEY),
			ao2_cleanup);
	struct ast_var_t *vardata;
	struct cdr_object *it_cdr;

	if (!cdr) {
		return -1;
	}

	ao2_lock(cdr);
	for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
		/* clear variables */
		if (!ast_test_flag(options, AST_CDR_FLAG_KEEP_VARS)) {
			while ((vardata = AST_LIST_REMOVE_HEAD(&it_cdr->party_a.variables, entries))) {
				ast_var_delete(vardata);
			}
			if (cdr->party_b.snapshot) {
				while ((vardata = AST_LIST_REMOVE_HEAD(&it_cdr->party_b.variables, entries))) {
					ast_var_delete(vardata);
				}
			}
		}

		/* Reset to initial state */
		memset(&it_cdr->start, 0, sizeof(it_cdr->start));
		memset(&it_cdr->end, 0, sizeof(it_cdr->end));
		memset(&it_cdr->answer, 0, sizeof(it_cdr->answer));
		it_cdr->start = ast_tvnow();
		cdr_object_check_party_a_answer(it_cdr);
	}
	ao2_unlock(cdr);

	return 0;
}

int ast_cdr_fork(const char *channel_name, struct ast_flags *options)
{
	RAII_VAR(struct cdr_object *, cdr,
			ao2_find(active_cdrs_by_channel, channel_name, OBJ_KEY),
			ao2_cleanup);
	struct cdr_object *new_cdr;
	struct cdr_object *it_cdr;
	struct cdr_object *cdr_obj;

	if (!cdr) {
		return -1;
	}

	{
		SCOPED_AO2LOCK(lock, cdr);
		cdr_obj = cdr->last;
		if (cdr_obj->fn_table == &finalized_state_fn_table) {
			/* If the last CDR in the chain is finalized, don't allow a fork -
			 * things are already dying at this point
			 */
			ast_log(AST_LOG_ERROR, "FARK\n");
			return -1;
		}

		/* Copy over the basic CDR information. The Party A information is
		 * copied over automatically as part of the append
		 */
		ast_debug(1, "Forking CDR for channel %s\n", cdr->party_a.snapshot->name);
		new_cdr = cdr_object_create_and_append(cdr);
		if (!new_cdr) {
			return -1;
		}
		new_cdr->fn_table = cdr_obj->fn_table;
		ast_string_field_set(new_cdr, bridge, cdr->bridge);
		new_cdr->flags = cdr->flags;

		/* If there's a Party B, copy it over as well */
		if (cdr_obj->party_b.snapshot) {
			new_cdr->party_b.snapshot = cdr_obj->party_b.snapshot;
			ao2_ref(new_cdr->party_b.snapshot, +1);
			strcpy(new_cdr->party_b.userfield, cdr_obj->party_b.userfield);
			new_cdr->party_b.flags = cdr_obj->party_b.flags;
			if (ast_test_flag(options, AST_CDR_FLAG_KEEP_VARS)) {
				copy_variables(&new_cdr->party_b.variables, &cdr_obj->party_b.variables);
			}
		}
		new_cdr->start = cdr_obj->start;
		new_cdr->answer = cdr_obj->answer;

		/* Modify the times based on the flags passed in */
		if (ast_test_flag(options, AST_CDR_FLAG_SET_ANSWER)
				&& new_cdr->party_a.snapshot->state == AST_STATE_UP) {
			new_cdr->answer = ast_tvnow();
		}
		if (ast_test_flag(options, AST_CDR_FLAG_RESET)) {
			new_cdr->answer = ast_tvnow();
			new_cdr->start = ast_tvnow();
		}

		/* Create and append, by default, copies over the variables */
		if (!ast_test_flag(options, AST_CDR_FLAG_KEEP_VARS)) {
			free_variables(&new_cdr->party_a.variables);
		}

		/* Finalize any current CDRs */
		if (ast_test_flag(options, AST_CDR_FLAG_FINALIZE)) {
			for (it_cdr = cdr; it_cdr != new_cdr; it_cdr = it_cdr->next) {
				if (it_cdr->fn_table == &finalized_state_fn_table) {
					continue;
				}
				/* Force finalization on the CDR. This will bypass any checks for
				 * end before 'h' extension.
				 */
				cdr_object_finalize(it_cdr);
				cdr_object_transition_state(it_cdr, &finalized_state_fn_table);
			}
		}
	}

	return 0;
}

/*! \note Don't call without cdr_batch_lock */
static void reset_batch(void)
{
	batch->size = 0;
	batch->head = NULL;
	batch->tail = NULL;
}

/*! \note Don't call without cdr_batch_lock */
static int init_batch(void)
{
	/* This is the single meta-batch used to keep track of all CDRs during the entire life of the program */
	if (!(batch = ast_malloc(sizeof(*batch))))
		return -1;

	reset_batch();

	return 0;
}

static void *do_batch_backend_process(void *data)
{
	struct cdr_batch_item *processeditem;
	struct cdr_batch_item *batchitem = data;

	/* Push each CDR into storage mechanism(s) and free all the memory */
	while (batchitem) {
		post_cdr(batchitem->cdr);
		ast_cdr_free(batchitem->cdr);
		processeditem = batchitem;
		batchitem = batchitem->next;
		ast_free(processeditem);
	}

	return NULL;
}

static void cdr_submit_batch(int do_shutdown)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);
	struct cdr_batch_item *oldbatchitems = NULL;
	pthread_t batch_post_thread = AST_PTHREADT_NULL;

	/* if there's no batch, or no CDRs in the batch, then there's nothing to do */
	if (!batch || !batch->head) {
		return;
	}

	/* move the old CDRs aside, and prepare a new CDR batch */
	ast_mutex_lock(&cdr_batch_lock);
	oldbatchitems = batch->head;
	reset_batch();
	ast_mutex_unlock(&cdr_batch_lock);

	/* if configured, spawn a new thread to post these CDRs,
	   also try to save as much as possible if we are shutting down safely */
	if (ast_test_flag(&mod_cfg->general->batch_settings.settings, BATCH_MODE_SCHEDULER_ONLY) || do_shutdown) {
		ast_debug(1, "CDR single-threaded batch processing begins now\n");
		do_batch_backend_process(oldbatchitems);
	} else {
		if (ast_pthread_create_detached_background(&batch_post_thread, NULL, do_batch_backend_process, oldbatchitems)) {
			ast_log(LOG_WARNING, "CDR processing thread could not detach, now trying in this thread\n");
			do_batch_backend_process(oldbatchitems);
		} else {
			ast_debug(1, "CDR multi-threaded batch processing begins now\n");
		}
	}
}

static int submit_scheduled_batch(const void *data)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);
	cdr_submit_batch(0);
	/* manually reschedule from this point in time */

	ast_mutex_lock(&cdr_sched_lock);
	cdr_sched = ast_sched_add(sched, mod_cfg->general->batch_settings.size * 1000, submit_scheduled_batch, NULL);
	ast_mutex_unlock(&cdr_sched_lock);
	/* returning zero so the scheduler does not automatically reschedule */
	return 0;
}

/*! Do not hold the batch lock while calling this function */
static void submit_unscheduled_batch(void)
{
	/* Prevent two deletes from happening at the same time */
	ast_mutex_lock(&cdr_sched_lock);
	/* this is okay since we are not being called from within the scheduler */
	AST_SCHED_DEL(sched, cdr_sched);
	/* schedule the submission to occur ASAP (1 ms) */
	cdr_sched = ast_sched_add(sched, 1, submit_scheduled_batch, NULL);
	ast_mutex_unlock(&cdr_sched_lock);

	/* signal the do_cdr thread to wakeup early and do some work (that lazy thread ;) */
	ast_mutex_lock(&cdr_pending_lock);
	ast_cond_signal(&cdr_pending_cond);
	ast_mutex_unlock(&cdr_pending_lock);
}

static void cdr_detach(struct ast_cdr *cdr)
{
	struct cdr_batch_item *newtail;
	int curr;
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);
	int submit_batch = 0;

	if (!cdr) {
		return;
	}

	/* maybe they disabled CDR stuff completely, so just drop it */
	if (!ast_test_flag(&mod_cfg->general->settings, CDR_ENABLED)) {
		ast_debug(1, "Dropping CDR !\n");
		ast_cdr_free(cdr);
		return;
	}

	/* post stuff immediately if we are not in batch mode, this is legacy behaviour */
	if (!ast_test_flag(&mod_cfg->general->settings, CDR_BATCHMODE)) {
		post_cdr(cdr);
		ast_cdr_free(cdr);
		return;
	}

	/* otherwise, each CDR gets put into a batch list (at the end) */
	ast_debug(1, "CDR detaching from this thread\n");

	/* we'll need a new tail for every CDR */
	if (!(newtail = ast_calloc(1, sizeof(*newtail)))) {
		post_cdr(cdr);
		ast_cdr_free(cdr);
		return;
	}

	/* don't traverse a whole list (just keep track of the tail) */
	ast_mutex_lock(&cdr_batch_lock);
	if (!batch)
		init_batch();
	if (!batch->head) {
		/* new batch is empty, so point the head at the new tail */
		batch->head = newtail;
	} else {
		/* already got a batch with something in it, so just append a new tail */
		batch->tail->next = newtail;
	}
	newtail->cdr = cdr;
	batch->tail = newtail;
	curr = batch->size++;

	/* if we have enough stuff to post, then do it */
	if (curr >= (mod_cfg->general->batch_settings.size - 1)) {
		submit_batch = 1;
	}
	ast_mutex_unlock(&cdr_batch_lock);

	/* Don't call submit_unscheduled_batch with the cdr_batch_lock held */
	if (submit_batch) {
		submit_unscheduled_batch();
	}
}

static void *do_cdr(void *data)
{
	struct timespec timeout;
	int schedms;
	int numevents = 0;

	for (;;) {
		struct timeval now;
		schedms = ast_sched_wait(sched);
		/* this shouldn't happen, but provide a 1 second default just in case */
		if (schedms <= 0)
			schedms = 1000;
		now = ast_tvadd(ast_tvnow(), ast_samp2tv(schedms, 1000));
		timeout.tv_sec = now.tv_sec;
		timeout.tv_nsec = now.tv_usec * 1000;
		/* prevent stuff from clobbering cdr_pending_cond, then wait on signals sent to it until the timeout expires */
		ast_mutex_lock(&cdr_pending_lock);
		ast_cond_timedwait(&cdr_pending_cond, &cdr_pending_lock, &timeout);
		numevents = ast_sched_runq(sched);
		ast_mutex_unlock(&cdr_pending_lock);
		ast_debug(2, "Processed %d scheduled CDR batches from the run queue\n", numevents);
	}

	return NULL;
}

static char *handle_cli_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);

	switch (cmd) {
	case CLI_INIT:
		e->command = "cdr set debug [on|off]";
		e->usage = "Enable or disable extra debugging in the CDR Engine";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 4) {
		return CLI_SHOWUSAGE;
	}

	if (!strcmp(a->argv[3], "on") && !ast_test_flag(&mod_cfg->general->settings, CDR_DEBUG)) {
		ast_set_flag(&mod_cfg->general->settings, CDR_DEBUG);
		ast_cli(a->fd, "CDR debugging enabled\n");
	} else if (!strcmp(a->argv[3], "off") && ast_test_flag(&mod_cfg->general->settings, CDR_DEBUG)) {
		ast_clear_flag(&mod_cfg->general->settings, CDR_DEBUG);
		ast_cli(a->fd, "CDR debugging disabled\n");
	}

	return CLI_SUCCESS;
}

static char *handle_cli_status(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct cdr_beitem *beitem = NULL;
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);
	int cnt = 0;
	long nextbatchtime = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "cdr show status";
		e->usage =
			"Usage: cdr show status\n"
			"	Displays the Call Detail Record engine system status.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc > 3)
		return CLI_SHOWUSAGE;

	ast_cli(a->fd, "\n");
	ast_cli(a->fd, "Call Detail Record (CDR) settings\n");
	ast_cli(a->fd, "----------------------------------\n");
	ast_cli(a->fd, "  Logging:                    %s\n", ast_test_flag(&mod_cfg->general->settings, CDR_ENABLED) ? "Enabled" : "Disabled");
	ast_cli(a->fd, "  Mode:                       %s\n", ast_test_flag(&mod_cfg->general->settings, CDR_BATCHMODE) ? "Batch" : "Simple");
	if (ast_test_flag(&mod_cfg->general->settings, CDR_ENABLED)) {
		ast_cli(a->fd, "  Log unanswered calls:       %s\n", ast_test_flag(&mod_cfg->general->settings, CDR_UNANSWERED) ? "Yes" : "No");
		ast_cli(a->fd, "  Log congestion:             %s\n\n", ast_test_flag(&mod_cfg->general->settings, CDR_CONGESTION) ? "Yes" : "No");
		if (ast_test_flag(&mod_cfg->general->settings, CDR_BATCHMODE)) {
			ast_cli(a->fd, "* Batch Mode Settings\n");
			ast_cli(a->fd, "  -------------------\n");
			if (batch)
				cnt = batch->size;
			if (cdr_sched > -1)
				nextbatchtime = ast_sched_when(sched, cdr_sched);
			ast_cli(a->fd, "  Safe shutdown:              %s\n", ast_test_flag(&mod_cfg->general->batch_settings.settings, BATCH_MODE_SAFE_SHUTDOWN) ? "Enabled" : "Disabled");
			ast_cli(a->fd, "  Threading model:            %s\n", ast_test_flag(&mod_cfg->general->batch_settings.settings, BATCH_MODE_SCHEDULER_ONLY) ? "Scheduler only" : "Scheduler plus separate threads");
			ast_cli(a->fd, "  Current batch size:         %d record%s\n", cnt, ESS(cnt));
			ast_cli(a->fd, "  Maximum batch size:         %d record%s\n", mod_cfg->general->batch_settings.size, ESS(mod_cfg->general->batch_settings.size));
			ast_cli(a->fd, "  Maximum batch time:         %d second%s\n", mod_cfg->general->batch_settings.time, ESS(mod_cfg->general->batch_settings.time));
			ast_cli(a->fd, "  Next batch processing time: %ld second%s\n\n", nextbatchtime, ESS(nextbatchtime));
		}
		ast_cli(a->fd, "* Registered Backends\n");
		ast_cli(a->fd, "  -------------------\n");
		AST_RWLIST_RDLOCK(&be_list);
		if (AST_RWLIST_EMPTY(&be_list)) {
			ast_cli(a->fd, "    (none)\n");
		} else {
			AST_RWLIST_TRAVERSE(&be_list, beitem, list) {
				ast_cli(a->fd, "    %s\n", beitem->name);
			}
		}
		AST_RWLIST_UNLOCK(&be_list);
		ast_cli(a->fd, "\n");
	}

	return CLI_SUCCESS;
}

static char *handle_cli_submit(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "cdr submit";
		e->usage =
			"Usage: cdr submit\n"
			"       Posts all pending batched CDR data to the configured CDR backend engine modules.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc > 2)
		return CLI_SHOWUSAGE;

	submit_unscheduled_batch();
	ast_cli(a->fd, "Submitted CDRs to backend engines for processing.  This may take a while.\n");

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_submit = AST_CLI_DEFINE(handle_cli_submit, "Posts all pending batched CDR data");
static struct ast_cli_entry cli_status = AST_CLI_DEFINE(handle_cli_status, "Display the CDR status");
static struct ast_cli_entry cli_debug = AST_CLI_DEFINE(handle_cli_debug, "Enable debugging");


/*!
 * \brief This dispatches *all* \ref cdr_objects. It should only be used during
 * shutdown, so that we get billing records for everything that we can.
 */
static int cdr_object_dispatch_all_cb(void *obj, void *arg, int flags)
{
	struct cdr_object *cdr = obj;
	struct cdr_object *it_cdr;

	for (it_cdr = cdr; it_cdr; it_cdr = it_cdr->next) {
		cdr_object_transition_state(it_cdr, &finalized_state_fn_table);
	}
	cdr_object_dispatch(cdr);

	return 0;
}

static void finalize_batch_mode(void)
{
	if (cdr_thread == AST_PTHREADT_NULL) {
		return;
	}
	/* wake up the thread so it will exit */
	pthread_cancel(cdr_thread);
	pthread_kill(cdr_thread, SIGURG);
	pthread_join(cdr_thread, NULL);
	cdr_thread = AST_PTHREADT_NULL;
	ast_cond_destroy(&cdr_pending_cond);
	ast_cli_unregister(&cli_submit);
	ast_cdr_engine_term();
}

static int process_config(int reload)
{
	RAII_VAR(struct module_config *, mod_cfg, module_config_alloc(), ao2_cleanup);

	if (!reload) {
		if (aco_info_init(&cfg_info)) {
			return 1;
		}

		aco_option_register(&cfg_info, "enable", ACO_EXACT, general_options, DEFAULT_ENABLED, OPT_BOOLFLAG_T, 1, FLDSET(struct ast_cdr_config, settings), CDR_ENABLED);
		aco_option_register(&cfg_info, "debug", ACO_EXACT, general_options, 0, OPT_BOOLFLAG_T, 1, FLDSET(struct ast_cdr_config, settings), CDR_DEBUG);
		aco_option_register(&cfg_info, "unanswered", ACO_EXACT, general_options, DEFAULT_UNANSWERED, OPT_BOOLFLAG_T, 1, FLDSET(struct ast_cdr_config, settings), CDR_UNANSWERED);
		aco_option_register(&cfg_info, "congestion", ACO_EXACT, general_options, 0, OPT_BOOLFLAG_T, 1, FLDSET(struct ast_cdr_config, settings), CDR_CONGESTION);
		aco_option_register(&cfg_info, "batch", ACO_EXACT, general_options, DEFAULT_BATCHMODE, OPT_BOOLFLAG_T, 1, FLDSET(struct ast_cdr_config, settings), CDR_BATCHMODE);
		aco_option_register(&cfg_info, "endbeforehexten", ACO_EXACT, general_options, DEFAULT_END_BEFORE_H_EXTEN, OPT_BOOLFLAG_T, 1, FLDSET(struct ast_cdr_config, settings), CDR_END_BEFORE_H_EXTEN);
		aco_option_register(&cfg_info, "initiatedseconds", ACO_EXACT, general_options, DEFAULT_INITIATED_SECONDS, OPT_BOOLFLAG_T, 1, FLDSET(struct ast_cdr_config, settings), CDR_INITIATED_SECONDS);
		aco_option_register(&cfg_info, "scheduleronly", ACO_EXACT, general_options, DEFAULT_BATCH_SCHEDULER_ONLY, OPT_BOOLFLAG_T, 1, FLDSET(struct ast_cdr_config, batch_settings.settings), BATCH_MODE_SCHEDULER_ONLY);
		aco_option_register(&cfg_info, "safeshutdown", ACO_EXACT, general_options, DEFAULT_BATCH_SAFE_SHUTDOWN, OPT_BOOLFLAG_T, 1, FLDSET(struct ast_cdr_config, batch_settings.settings), BATCH_MODE_SAFE_SHUTDOWN);
		aco_option_register(&cfg_info, "size", ACO_EXACT, general_options, DEFAULT_BATCH_SIZE, OPT_UINT_T, PARSE_IN_RANGE, FLDSET(struct ast_cdr_config, batch_settings.size), 0, MAX_BATCH_SIZE);
		aco_option_register(&cfg_info, "time", ACO_EXACT, general_options, DEFAULT_BATCH_TIME, OPT_UINT_T, PARSE_IN_RANGE, FLDSET(struct ast_cdr_config, batch_settings.time), 0, MAX_BATCH_TIME);
	}

	if (aco_process_config(&cfg_info, reload)) {
		if (!mod_cfg) {
			return 1;
		}
		/* If we couldn't process the configuration and this wasn't a reload,
		 * create a default config
		 */
		if (!reload && !(aco_set_defaults(&general_option, "general", mod_cfg->general))) {
			ast_log(LOG_NOTICE, "Failed to process CDR configuration; using defaults\n");
			ao2_global_obj_replace(module_configs, mod_cfg);
			return 0;
		}
		return 1;
	}

	if (reload) {
		manager_event(EVENT_FLAG_SYSTEM, "Reload", "Module: CDR\r\nMessage: CDR subsystem reload requested\r\n");
	}
	return 0;
}

static void cdr_engine_shutdown(void)
{
	ao2_callback(active_cdrs_by_channel, OBJ_NODATA, cdr_object_dispatch_all_cb,
		NULL);
	finalize_batch_mode();
	aco_info_destroy(&cfg_info);
	ast_cli_unregister(&cli_status);
	ast_cli_unregister(&cli_debug);
	ast_sched_context_destroy(sched);
	sched = NULL;
	ast_free(batch);
	batch = NULL;

	ao2_ref(active_cdrs_by_channel, -1);
	ao2_ref(active_cdrs_by_bridge, -1);
}

static void cdr_enable_batch_mode(struct ast_cdr_config *config)
{
	SCOPED_LOCK(batch, &cdr_batch_lock, ast_mutex_lock, ast_mutex_unlock);

	/* Only create the thread level portions once */
	if (cdr_thread == AST_PTHREADT_NULL) {
		ast_cond_init(&cdr_pending_cond, NULL);
		if (ast_pthread_create_background(&cdr_thread, NULL, do_cdr, NULL) < 0) {
			ast_log(LOG_ERROR, "Unable to start CDR thread.\n");
			return;
		}
		ast_cli_register(&cli_submit);
	}

	/* Kill the currently scheduled item */
	AST_SCHED_DEL(sched, cdr_sched);
	cdr_sched = ast_sched_add(sched, config->batch_settings.time * 1000, submit_scheduled_batch, NULL);
	ast_log(LOG_NOTICE, "CDR batch mode logging enabled, first of either size %d or time %d seconds.\n",
			config->batch_settings.size, config->batch_settings.time);
}

int ast_cdr_engine_init(void)
{
	RAII_VAR(struct module_config *, mod_cfg, NULL, ao2_cleanup);

	if (process_config(0)) {
		return -1;
	}

	/* The prime here should be the same as the channel container */
	active_cdrs_by_channel = ao2_container_alloc(51, cdr_object_channel_hash_fn, cdr_object_channel_cmp_fn);
	if (!active_cdrs_by_channel) {
		return -1;
	}

	active_cdrs_by_bridge = ao2_container_alloc(51, cdr_object_bridge_hash_fn, cdr_object_bridge_cmp_fn);
	if (!active_cdrs_by_bridge) {
		return -1;
	}

	cdr_topic = stasis_topic_create("cdr_engine");
	if (!cdr_topic) {
		return -1;
	}

	channel_subscription = stasis_forward_all(stasis_caching_get_topic(ast_channel_topic_all_cached()), cdr_topic);
	if (!channel_subscription) {
		return -1;
	}
	bridge_subscription = stasis_forward_all(stasis_caching_get_topic(ast_bridge_topic_all_cached()), cdr_topic);
	if (!bridge_subscription) {
		return -1;
	}
	stasis_router = stasis_message_router_create(cdr_topic);
	if (!stasis_router) {
		return -1;
	}
	stasis_message_router_add(stasis_router, stasis_cache_update_type(), handle_channel_cache_message, NULL);
	stasis_message_router_add(stasis_router, ast_channel_dial_type(), handle_dial_message, NULL);
	stasis_message_router_add(stasis_router, ast_channel_entered_bridge_type(), handle_bridge_enter_message, NULL);
	stasis_message_router_add(stasis_router, ast_channel_left_bridge_type(), handle_bridge_leave_message, NULL);

	sched = ast_sched_context_create();
	if (!sched) {
		ast_log(LOG_ERROR, "Unable to create schedule context.\n");
		return -1;
	}

	ast_cli_register(&cli_status);
	ast_cli_register(&cli_debug);
	ast_register_atexit(cdr_engine_shutdown);

	mod_cfg = ao2_global_obj_ref(module_configs);

	if (ast_test_flag(&mod_cfg->general->settings, CDR_ENABLED)) {
		if (ast_test_flag(&mod_cfg->general->settings, CDR_BATCHMODE)) {
			cdr_enable_batch_mode(mod_cfg->general);
		} else {
			ast_log(LOG_NOTICE, "CDR simple logging enabled.\n");
		}
	} else {
		ast_log(LOG_NOTICE, "CDR logging disabled.\n");
	}

	return 0;
}

void ast_cdr_engine_term(void)
{
	RAII_VAR(struct module_config *, mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);

	/* Since this is called explicitly during process shutdown, we might not have ever
	 * been initialized. If so, the config object will be NULL.
	 */
	if (!mod_cfg) {
		return;
	}
	if (!ast_test_flag(&mod_cfg->general->settings, CDR_BATCHMODE)) {
		return;
	}
	cdr_submit_batch(ast_test_flag(&mod_cfg->general->batch_settings.settings, BATCH_MODE_SAFE_SHUTDOWN));
}

int ast_cdr_engine_reload(void)
{
	RAII_VAR(struct module_config *, old_mod_cfg, ao2_global_obj_ref(module_configs), ao2_cleanup);
	RAII_VAR(struct module_config *, mod_cfg, NULL, ao2_cleanup);

	if (process_config(1)) {
		return -1;
	}

	mod_cfg = ao2_global_obj_ref(module_configs);

	if (!ast_test_flag(&mod_cfg->general->settings, CDR_ENABLED) ||
			!(ast_test_flag(&mod_cfg->general->settings, CDR_BATCHMODE))) {
		/* If batch mode used to be enabled, finalize the batch */
		if (ast_test_flag(&old_mod_cfg->general->settings, CDR_BATCHMODE)) {
			finalize_batch_mode();
		}
	}

	if (ast_test_flag(&mod_cfg->general->settings, CDR_ENABLED)) {
		if (!ast_test_flag(&mod_cfg->general->settings, CDR_BATCHMODE)) {
			ast_log(LOG_NOTICE, "CDR simple logging enabled.\n");
		} else {
			cdr_enable_batch_mode(mod_cfg->general);
		}
	} else {
		ast_log(LOG_NOTICE, "CDR logging disabled, data will be lost.\n");
	}

	return 0;
}


