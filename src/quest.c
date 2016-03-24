/* ************************************************************************
*   File: quest.c                                         EmpireMUD 2.0b3 *
*  Usage: quest loading, saving, OLC, and processing                      *
*                                                                         *
*  EmpireMUD code base by Paul Clarke, (C) 2000-2015                      *
*  All rights reserved.  See license.doc for complete information.        *
*                                                                         *
*  EmpireMUD based upon CircleMUD 3.0, bpl 17, by Jeremy Elson.           *
*  CircleMUD (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
************************************************************************ */

#include <math.h>

#include "conf.h"
#include "sysdep.h"

#include "structs.h"
#include "utils.h"
#include "interpreter.h"
#include "db.h"
#include "comm.h"
#include "olc.h"
#include "skills.h"
#include "handler.h"
#include "dg_scripts.h"
#include "vnums.h"

/**
* Contents:
*   Helpers
*   Utilities
*   Database
*   OLC Handlers
*   Displays
*   Edit Modules
*/

// local data
const char *default_quest_name = "Unnamed Quest";
const char *default_quest_description = "This quest has no description.\r\n";
const char *default_quest_complete_msg = "You have completed the quest.\r\n";

// external consts
extern const char *action_bits[];
extern const char *component_flags[];
extern const char *component_types[];
extern const char *quest_flags[];
extern const char *quest_giver_types[];
extern const char *quest_reward_types[];
extern const char *quest_tracker_types[];

// external funcs
void get_script_display(struct trig_proto_list *list, char *save_buffer);

// local protos


 //////////////////////////////////////////////////////////////////////////////
//// HELPERS /////////////////////////////////////////////////////////////////

/**
* Quick way to turn a vnum into a name, safely.
*
* @param any_vnum vnum The quest vnum to look up.
* @return char* A name for the vnum, or "UNKNOWN".
*/
char *get_quest_name_by_proto(any_vnum vnum) {
	quest_data *proto = quest_proto(vnum);
	return proto ? QUEST_NAME(proto) : "UNKNOWN";
}


 //////////////////////////////////////////////////////////////////////////////
//// UTILITIES ///////////////////////////////////////////////////////////////

/**
* Checks for common quest problems and reports them to ch.
*
* @param quest_data *quest The item to audit.
* @param char_data *ch The person to report to.
* @return bool TRUE if any problems were reported; FALSE if all good.
*/
bool audit_quest(quest_data *quest, char_data *ch) {
	bool problem = FALSE;
	
	if (QUEST_FLAGGED(quest, QST_IN_DEVELOPMENT)) {
		olc_audit_msg(ch, QUEST_VNUM(quest), "IN-DEVELOPMENT");
		problem = TRUE;
	}
	if (!QUEST_NAME(quest) || !*QUEST_NAME(quest) || !str_cmp(QUEST_NAME(quest), default_quest_name)) {
		olc_audit_msg(ch, QUEST_VNUM(quest), "Name not set");
		problem = TRUE;
	}
	
	return problem;
}


/**
* For the .list command.
*
* @param quest_data *quest The thing to list.
* @param bool detail If TRUE, provide additional details
* @return char* The line to show (without a CRLF).
*/
char *list_one_quest(quest_data *quest, bool detail) {
	static char output[MAX_STRING_LENGTH];
	
	if (detail) {
		snprintf(output, sizeof(output), "[%5d] %s", QUEST_VNUM(quest), QUEST_NAME(quest));
	}
	else {
		snprintf(output, sizeof(output), "[%5d] %s", QUEST_VNUM(quest), QUEST_NAME(quest));
	}
		
	return output;
}


/**
* Searches for all uses of a quest and displays them.
*
* @param char_data *ch The player.
* @param any_vnum vnum The quest vnum.
*/
void olc_search_quest(char_data *ch, any_vnum vnum) {
	char buf[MAX_STRING_LENGTH];
	quest_data *quest = quest_proto(vnum);
	int size, found;
	
	if (!quest) {
		msg_to_char(ch, "There is no quest %d.\r\n", vnum);
		return;
	}
	
	found = 0;
	size = snprintf(buf, sizeof(buf), "Occurrences of quest %d (%s):\r\n", vnum, QUEST_NAME(quest));
	
	// quests are not actually used anywhere else
	
	if (found > 0) {
		size += snprintf(buf + size, sizeof(buf) - size, "%d location%s shown\r\n", found, PLURAL(found));
	}
	else {
		size += snprintf(buf + size, sizeof(buf) - size, " none\r\n");
	}
	
	page_string(ch->desc, buf, TRUE);
}


// Simple vnum sorter for the quest hash
int sort_quests(quest_data *a, quest_data *b) {
	return QUEST_VNUM(a) - QUEST_VNUM(b);
}


 //////////////////////////////////////////////////////////////////////////////
//// DATABASE ////////////////////////////////////////////////////////////////

/**
* @param any_vnum vnum Any quest vnum
* @return quest_data* The quest, or NULL if it doesn't exist
*/
quest_data *quest_proto(any_vnum vnum) {
	quest_data *quest;
	
	if (vnum < 0 || vnum == NOTHING) {
		return NULL;
	}
	
	HASH_FIND_INT(quest_table, &vnum, quest);
	return quest;
}


/**
* Puts a quest into the hash table.
*
* @param quest_data *quest The quest data to add to the table.
*/
void add_quest_to_table(quest_data *quest) {
	quest_data *find;
	any_vnum vnum;
	
	if (quest) {
		vnum = QUEST_VNUM(quest);
		HASH_FIND_INT(quest_table, &vnum, find);
		if (!find) {
			HASH_ADD_INT(quest_table, vnum, quest);
			HASH_SORT(quest_table, sort_quests);
		}
	}
}


/**
* Removes a quest from the hash table.
*
* @param quest_data *quest The quest data to remove from the table.
*/
void remove_quest_from_table(quest_data *quest) {
	HASH_DEL(quest_table, quest);
}


/**
* Initializes a new quest. This clears all memory for it, so set the vnum
* AFTER.
*
* @param quest_data *quest The quest to initialize.
*/
void clear_quest(quest_data *quest) {
	memset((char *) quest, 0, sizeof(quest_data));
	
	QUEST_VNUM(quest) = NOTHING;
	QUEST_REPEATABLE_AFTER(quest) = NOT_REPEATABLE;
}


/**
* @param struct quest_giver *from The list to copy.
* @return struct quest_giver* The copy of the list.
*/
struct quest_giver *copy_quest_givers(struct quest_giver *from) {
	struct quest_giver *el, *iter, *list = NULL, *end = NULL;
	
	LL_FOREACH(from, iter) {
		CREATE(el, struct quest_giver, 1);
		*el = *iter;
		el->next = NULL;
		
		if (end) {
			end->next = el;
		}
		else {
			list = el;
		}
		end = el;
	}
	
	return list;
}


/**
* @param struct quest_reward *from The list to copy.
* @return struct quest_reward* The copy of the list.
*/
struct quest_reward *copy_quest_rewards(struct quest_reward *from) {
	struct quest_reward *el, *iter, *list = NULL, *end = NULL;
	
	LL_FOREACH(from, iter) {
		CREATE(el, struct quest_reward, 1);
		*el = *iter;
		el->next = NULL;
		
		if (end) {
			end->next = el;
		}
		else {
			list = el;
		}
		end = el;
	}
	
	return list;
}


/**
* @param struct quest_task *from The list to copy.
* @return struct quest_task* The copy of the list.
*/
struct quest_task *copy_quest_tasks(struct quest_task *from) {
	struct quest_task *el, *iter, *list = NULL, *end = NULL;
	
	LL_FOREACH(from, iter) {
		CREATE(el, struct quest_task, 1);
		*el = *iter;
		el->next = NULL;
		
		if (end) {
			end->next = el;
		}
		else {
			list = el;
		}
		end = el;
	}
	
	return list;
}


/**
* @param struct quest_giver *list The list to free.
*/
void free_quest_givers(struct quest_giver *list) {
	struct quest_giver *iter, *next_iter;
	LL_FOREACH_SAFE(list, iter, next_iter) {
		free(iter);
	}
}


/**
* @param struct quest_reward *list The list to free.
*/
void free_quest_rewards(struct quest_reward *list) {
	struct quest_reward *iter, *next_iter;
	LL_FOREACH_SAFE(list, iter, next_iter) {
		free(iter);
	}
}


/**
* @param struct quest_task *list The list to free.
*/
void free_quest_tasks(struct quest_task *list) {
	struct quest_task *iter, *next_iter;
	LL_FOREACH_SAFE(list, iter, next_iter) {
		free(iter);
	}
}


/**
* frees up memory for a quest data item.
*
* See also: olc_delete_quest
*
* @param quest_data *quest The quest data to free.
*/
void free_quest(quest_data *quest) {
	quest_data *proto = quest_proto(QUEST_VNUM(quest));
	
	// strings
	if (QUEST_NAME(quest) && (!proto || QUEST_NAME(quest) != QUEST_NAME(proto))) {
		free(QUEST_NAME(quest));
	}
	if (QUEST_DESCRIPTION(quest) && (!proto || QUEST_DESCRIPTION(quest) != QUEST_DESCRIPTION(proto))) {
		free(QUEST_DESCRIPTION(quest));
	}
	if (QUEST_COMPLETE_MSG(quest) && (!proto || QUEST_COMPLETE_MSG(quest) != QUEST_COMPLETE_MSG(proto))) {
		free(QUEST_COMPLETE_MSG(quest));
	}
	
	// pointers
	if (QUEST_STARTS_AT(quest) && (!proto || QUEST_STARTS_AT(quest) != QUEST_STARTS_AT(proto))) {
		free_quest_givers(QUEST_STARTS_AT(quest));
	}
	if (QUEST_ENDS_AT(quest) && (!proto || QUEST_ENDS_AT(quest) != QUEST_ENDS_AT(proto))) {
		free_quest_givers(QUEST_ENDS_AT(quest));
	}
	if (QUEST_TASKS(quest) && (!proto || QUEST_TASKS(quest) != QUEST_TASKS(proto))) {
		free_quest_tasks(QUEST_TASKS(quest));
	}
	if (QUEST_REWARDS(quest) && (!proto || QUEST_REWARDS(quest) != QUEST_REWARDS(proto))) {
		free_quest_rewards(QUEST_REWARDS(quest));
	}
	if (QUEST_PREREQS(quest) && (!proto || QUEST_PREREQS(quest) != QUEST_PREREQS(proto))) {
		free_quest_tasks(QUEST_PREREQS(quest));
	}
	if (QUEST_SCRIPTS(quest) && (!proto || QUEST_SCRIPTS(quest) != QUEST_SCRIPTS(proto))) {
		free_proto_scripts(&QUEST_SCRIPTS(quest));
	}
	
	free(quest);
}


/**
* Parses a quest giver, saved as:
*
* A
* 1 123
*
* @param FILE *fl The file, having just read the letter tag.
* @param struct quest_giver **list The list to append to.
* @param char *error_str How to report if there is an error.
*/
void parse_quest_giver(FILE *fl, struct quest_giver **list, char *error_str) {
	struct quest_giver *giver;
	char line[256];
	any_vnum vnum;
	int type;
	
	if (!fl || !list || !get_line(fl, line)) {
		log("SYSERR: data error in quest giver line of %s", error_str ? error_str : "UNKNOWN");
		exit(1);
	}
	if (sscanf(line, "%d %d", &type, &vnum) != 2) {
		log("SYSERR: format error in quest giver line of %s", error_str ? error_str : "UNKNOWN");
		exit(1);
	}
	
	CREATE(giver, struct quest_giver, 1);
	giver->type = type;
	giver->vnum = vnum;
	
	LL_APPEND(*list, giver);
}


/**
* Parses a quest reward, saved as:
*
* A
* 1 123 2
*
* @param FILE *fl The file, having just read the letter tag.
* @param struct quest_reward **list The list to append to.
* @param char *error_str How to report if there is an error.
*/
void parse_quest_reward(FILE *fl, struct quest_reward **list, char *error_str) {
	struct quest_reward *reward;
	int type, amount;
	char line[256];
	any_vnum vnum;
	
	if (!fl || !list || !get_line(fl, line)) {
		log("SYSERR: data error in quest reward line of %s", error_str ? error_str : "UNKNOWN");
		exit(1);
	}
	if (sscanf(line, "%d %d %d", &type, &vnum, &amount) != 3) {
		log("SYSERR: format error in quest reward line of %s", error_str ? error_str : "UNKNOWN");
		exit(1);
	}
	
	CREATE(reward, struct quest_reward, 1);
	reward->type = type;
	reward->vnum = vnum;
	reward->amount = amount;
	
	LL_APPEND(*list, reward);
}


/**
* Parses a quest task, saved as:
*
* A
* 1 123 123456 10
*
* @param FILE *fl The file, having just read the letter tag.
* @param struct quest_task **list The list to append to.
* @param char *error_str How to report if there is an error.
*/
void parse_quest_task(FILE *fl, struct quest_task **list, char *error_str) {
	struct quest_task *task;
	int type, needed;
	bitvector_t misc;
	char line[256];
	any_vnum vnum;
	
	if (!fl || !list || !get_line(fl, line)) {
		log("SYSERR: data error in quest task line of %s", error_str ? error_str : "UNKNOWN");
		exit(1);
	}
	if (sscanf(line, "%d %d %llu %d", &type, &vnum, &misc, &needed) != 4) {
		log("SYSERR: format error in quest task line of %s", error_str ? error_str : "UNKNOWN");
		exit(1);
	}
	
	CREATE(task, struct quest_task, 1);
	task->type = type;
	task->vnum = vnum;
	task->misc = misc;
	task->needed = needed;
	task->current = 0;
	
	LL_APPEND(*list, task);
}


/**
* Read one quest from file.
*
* @param FILE *fl The open .qst file
* @param any_vnum vnum The quest vnum
*/
void parse_quest(FILE *fl, any_vnum vnum) {
	char line[256], error[256], str_in[256];
	quest_data *quest, *find;
	int int_in[4];
	
	CREATE(quest, quest_data, 1);
	clear_quest(quest);
	QUEST_VNUM(quest) = vnum;
	
	HASH_FIND_INT(quest_table, &vnum, find);
	if (find) {
		log("WARNING: Duplicate quest vnum #%d", vnum);
		// but have to load it anyway to advance the file
	}
	add_quest_to_table(quest);
		
	// for error messages
	sprintf(error, "quest vnum %d", vnum);
	
	// lines 1-3: strings
	QUEST_NAME(quest) = fread_string(fl, error);
	QUEST_DESCRIPTION(quest) = fread_string(fl, error);
	QUEST_COMPLETE_MSG(quest) = fread_string(fl, error);
	
	// 4. flags min max repeatable-after
	if (!get_line(fl, line) || sscanf(line, "%s %d %d %d", str_in, &int_in[0], &int_in[1], &int_in[2]) != 4) {
		log("SYSERR: Format error in line 4 of %s", error);
		exit(1);
	}
	
	QUEST_FLAGS(quest) = asciiflag_conv(str_in);
	QUEST_MIN_LEVEL(quest) = int_in[0];
	QUEST_MAX_LEVEL(quest) = int_in[1];
	QUEST_REPEATABLE_AFTER(quest) = int_in[2];
	
	// optionals
	for (;;) {
		if (!get_line(fl, line)) {
			log("SYSERR: Format error in %s, expecting alphabetic flags", error);
			exit(1);
		}
		switch (*line) {
			case 'A': {	// starts at
				parse_quest_giver(fl, &QUEST_STARTS_AT(quest), error);
				break;
			}
			case 'P': {	// preq-requisites
				parse_quest_task(fl, &QUEST_PREREQS(quest), error);
				break;
			}
			case 'R': {	// rewards
				parse_quest_reward(fl, &QUEST_REWARDS(quest), error);
				break;
			}
			case 'T': {	// triggers
				parse_trig_proto(line, &QUEST_SCRIPTS(quest), error);
				break;
			}
			case 'W': {	// tasks / work
				parse_quest_task(fl, &QUEST_TASKS(quest), error);
				break;
			}
			case 'Z': {	// ends at
				parse_quest_giver(fl, &QUEST_ENDS_AT(quest), error);
				break;
			}
			
			// end
			case 'S': {
				return;
			}
			
			default: {
				log("SYSERR: Format error in %s, expecting alphabetic flags", error);
				exit(1);
			}
		}
	}
}


// writes entries in the quest index
void write_quest_index(FILE *fl) {
	quest_data *quest, *next_quest;
	int this, last;
	
	last = -1;
	HASH_ITER(hh, quest_table, quest, next_quest) {
		// determine "zone number" by vnum
		this = (int)(QUEST_VNUM(quest) / 100);
	
		if (this != last) {
			fprintf(fl, "%d%s\n", this, QST_SUFFIX);
			last = this;
		}
	}
}


/**
* Writes a list of 'quest_giver' to a data file.
*
* @param FILE *fl The file, open for writing.
* @param char letter The tag letter.
* @param struct quest_giver *list The list to write.
*/
void write_quest_givers_to_file(FILE *fl, char letter, struct quest_giver *list) {
	struct quest_giver *iter;
	LL_FOREACH(list, iter) {
		fprintf(fl, "%c\n%d %d\n", letter, iter->type, iter->vnum);
	}
}


/**
* Writes a list of 'quest_reward' to a data file.
*
* @param FILE *fl The file, open for writing.
* @param char letter The tag letter.
* @param struct quest_reward *list The list to write.
*/
void write_quest_rewards_to_file(FILE *fl, char letter, struct quest_reward *list) {
	struct quest_reward *iter;
	LL_FOREACH(list, iter) {
		fprintf(fl, "%c\n%d %d %d\n", letter, iter->type, iter->vnum, iter->amount);
	}
}


/**
* Writes a list of 'quest_task' to a data file.
*
* @param FILE *fl The file, open for writing.
* @param char letter The tag letter.
* @param struct quest_task *list The list to write.
*/
void write_quest_tasks_to_file(FILE *fl, char letter, struct quest_task *list) {
	struct quest_task *iter;
	LL_FOREACH(list, iter) {
		// NOTE: iter->current is NOT written to file
		fprintf(fl, "%c\n%d %d %llu %d\n", letter, iter->type, iter->vnum, iter->misc, iter->needed);
	}
}


/**
* Outputs one quest item in the db file format, starting with a #VNUM and
* ending with an S.
*
* @param FILE *fl The file to write it to.
* @param quest_data *quest The thing to save.
*/
void write_quest_to_file(FILE *fl, quest_data *quest) {
	void write_trig_protos_to_file(FILE *fl, char letter, struct trig_proto_list *list);
	
	char temp[MAX_STRING_LENGTH];
	
	if (!fl || !quest) {
		syslog(SYS_ERROR, LVL_START_IMM, TRUE, "SYSERR: write_quest_to_file called without %s", !fl ? "file" : "quest");
		return;
	}
	
	fprintf(fl, "#%d\n", QUEST_VNUM(quest));
	
	// 1. name
	fprintf(fl, "%s~\n", NULLSAFE(QUEST_NAME(quest)));
	
	// 2. desc
	strcpy(temp, NULLSAFE(QUEST_DESCRIPTION(quest)));
	strip_crlf(temp);
	fprintf(fl, "%s~\n", temp);
	
	// 3. complete msg
	strcpy(temp, NULLSAFE(QUEST_COMPLETE_MSG(quest)));
	strip_crlf(temp);
	fprintf(fl, "%s~\n", temp);
	
	// 4. flags min max repeatable-after
	fprintf(fl, "%s %d %d %d\n", bitv_to_alpha(QUEST_FLAGS(quest)), QUEST_MIN_LEVEL(quest), QUEST_MAX_LEVEL(quest), QUEST_REPEATABLE_AFTER(quest));
		
	// A. starts at
	write_quest_givers_to_file(fl, 'A', QUEST_STARTS_AT(quest));
	
	// P. pre-requisites
	write_quest_tasks_to_file(fl, 'P', QUEST_PREREQS(quest));
	
	// R. rewards
	write_quest_rewards_to_file(fl, 'R', QUEST_REWARDS(quest));
	
	// T. triggers
	write_trig_protos_to_file(fl, 'T', QUEST_SCRIPTS(quest));
	
	// W. tasks (work)
	write_quest_tasks_to_file(fl, 'W', QUEST_TASKS(quest));
	
	// Z. ends at
	write_quest_givers_to_file(fl, 'Z', QUEST_ENDS_AT(quest));
	
	// end
	fprintf(fl, "S\n");
}


 //////////////////////////////////////////////////////////////////////////////
//// OLC HANDLERS ////////////////////////////////////////////////////////////


/**
* Creates a new quest entry.
* 
* @param any_vnum vnum The number to create.
* @return quest_data* The new quest's prototype.
*/
quest_data *create_quest_table_entry(any_vnum vnum) {
	quest_data *quest;
	
	// sanity
	if (quest_proto(vnum)) {
		log("SYSERR: Attempting to insert quest at existing vnum %d", vnum);
		return quest_proto(vnum);
	}
	
	CREATE(quest, quest_data, 1);
	clear_quest(quest);
	QUEST_VNUM(quest) = vnum;
	QUEST_NAME(quest) = str_dup(default_quest_name);
	QUEST_DESCRIPTION(quest) = str_dup(default_quest_description);
	QUEST_COMPLETE_MSG(quest) = str_dup(default_quest_complete_msg);
	QUEST_FLAGS(quest) = QST_IN_DEVELOPMENT;
	add_quest_to_table(quest);

	// save index and quest file now
	save_index(DB_BOOT_QST);
	save_library_file_for_vnum(DB_BOOT_QST, vnum);

	return quest;
}


/**
* WARNING: This function actually deletes a quest.
*
* @param char_data *ch The person doing the deleting.
* @param any_vnum vnum The vnum to delete.
*/
void olc_delete_quest(char_data *ch, any_vnum vnum) {
	quest_data *quest;
	
	if (!(quest = quest_proto(vnum))) {
		msg_to_char(ch, "There is no such quest %d.\r\n", vnum);
		return;
	}
	
	// look for live copies of the quest
	// TODO
	
	// remove it from the hash table first
	remove_quest_from_table(quest);

	// save index and quest file now
	save_index(DB_BOOT_QST);
	save_library_file_for_vnum(DB_BOOT_QST, vnum);
	
	syslog(SYS_OLC, GET_INVIS_LEV(ch), TRUE, "OLC: %s has deleted quest %d", GET_NAME(ch), vnum);
	msg_to_char(ch, "Quest %d deleted.\r\n", vnum);
	
	free_quest(quest);
}


/**
* Function to save a player's changes to a quest (or a new one).
*
* @param descriptor_data *desc The descriptor who is saving.
*/
void save_olc_quest(descriptor_data *desc) {	
	quest_data *proto, *quest = GET_OLC_QUEST(desc);
	any_vnum vnum = GET_OLC_VNUM(desc);
	UT_hash_handle hh;

	// have a place to save it?
	if (!(proto = quest_proto(vnum))) {
		proto = create_quest_table_entry(vnum);
	}
	
	// free prototype strings and pointers
	if (QUEST_NAME(proto)) {
		free(QUEST_NAME(proto));
	}
	if (QUEST_DESCRIPTION(proto)) {
		free(QUEST_DESCRIPTION(proto));
	}
	if (QUEST_COMPLETE_MSG(proto)) {
		free(QUEST_COMPLETE_MSG(proto));
	}
	free_quest_givers(QUEST_STARTS_AT(proto));
	free_quest_givers(QUEST_ENDS_AT(proto));
	free_quest_tasks(QUEST_TASKS(proto));
	free_quest_rewards(QUEST_REWARDS(proto));
	free_quest_tasks(QUEST_PREREQS(proto));
	free_proto_scripts(&QUEST_SCRIPTS(proto));
	
	// sanity
	if (!QUEST_NAME(quest) || !*QUEST_NAME(quest)) {
		if (QUEST_NAME(quest)) {
			free(QUEST_NAME(quest));
		}
		QUEST_NAME(quest) = str_dup(default_quest_name);
	}
	if (!QUEST_DESCRIPTION(quest) || !*QUEST_DESCRIPTION(quest)) {
		if (QUEST_DESCRIPTION(quest)) {
			free(QUEST_DESCRIPTION(quest));
		}
		QUEST_DESCRIPTION(quest) = str_dup(default_quest_description);
	}
	if (!QUEST_COMPLETE_MSG(quest) || !*QUEST_COMPLETE_MSG(quest)) {
		if (QUEST_COMPLETE_MSG(quest)) {
			free(QUEST_COMPLETE_MSG(quest));
		}
		QUEST_COMPLETE_MSG(quest) = str_dup(default_quest_complete_msg);
	}
	
	// save data back over the proto-type
	hh = proto->hh;	// save old hash handle
	*proto = *quest;	// copy over all data
	proto->vnum = vnum;	// ensure correct vnum
	proto->hh = hh;	// restore old hash handle
		
	// and save to file
	save_library_file_for_vnum(DB_BOOT_QST, vnum);
}


/**
* Creates a copy of a quest, or clears a new one, for editing.
* 
* @param quest_data *input The quest to copy, or NULL to make a new one.
* @return quest_data* The copied quest.
*/
quest_data *setup_olc_quest(quest_data *input) {
	extern struct apply_data *copy_apply_list(struct apply_data *input);
	
	quest_data *new;
	
	CREATE(new, quest_data, 1);
	clear_quest(new);
	
	if (input) {
		// copy normal data
		*new = *input;

		// copy things that are pointers
		QUEST_NAME(new) = QUEST_NAME(input) ? str_dup(QUEST_NAME(input)) : NULL;
		QUEST_DESCRIPTION(new) = QUEST_DESCRIPTION(input) ? str_dup(QUEST_DESCRIPTION(input)) : NULL;
		QUEST_COMPLETE_MSG(new) = QUEST_COMPLETE_MSG(input) ? str_dup(QUEST_COMPLETE_MSG(input)) : NULL;
		
		QUEST_STARTS_AT(new) = copy_quest_givers(QUEST_STARTS_AT(input));
		QUEST_ENDS_AT(new) = copy_quest_givers(QUEST_ENDS_AT(input));
		QUEST_TASKS(new) = copy_quest_tasks(QUEST_TASKS(input));
		QUEST_REWARDS(new) = copy_quest_rewards(QUEST_REWARDS(input));
		QUEST_PREREQS(new) = copy_quest_tasks(QUEST_PREREQS(input));
		QUEST_SCRIPTS(new) = copy_trig_protos(QUEST_SCRIPTS(input));
	}
	else {
		// brand new: some defaults
		QUEST_NAME(new) = str_dup(default_quest_name);
		QUEST_DESCRIPTION(new) = str_dup(default_quest_description);
		QUEST_COMPLETE_MSG(new) = str_dup(default_quest_complete_msg);
		QUEST_FLAGS(new) = QST_IN_DEVELOPMENT;
	}
	
	// done
	return new;	
}


 //////////////////////////////////////////////////////////////////////////////
//// DISPLAYS ////////////////////////////////////////////////////////////////

/**
* Gets the display for a set of quest givers.
*
* @param struct quest_giver *list Pointer to the start of a list of quest givers.
* @param char *save_buffer A buffer to store the result to.
*/
void get_quest_giver_display(struct quest_giver *list, char *save_buffer) {
	char buf[MAX_STRING_LENGTH];
	struct quest_giver *giver;
	int count = 0;
	
	*save_buffer = '\0';
	LL_FOREACH(list, giver) {
		// QG_x
		switch (giver->type) {
			case QG_BUILDING: {
				bld_data *bld = building_proto(giver->vnum);
				strcpy(buf, bld ? GET_BLD_NAME(bld) : "UNKNOWN");
				break;
			}
			case QG_MOBILE: {
				strcpy(buf, get_mob_name_by_proto(giver->vnum));
				break;
			}
			case QG_OBJECT: {
				strcpy(buf, get_obj_name_by_proto(giver->vnum));
				break;
			}
			case QG_ROOM_TEMPLATE: {
				room_template *rmt = room_template_proto(giver->vnum);
				strcpy(buf, rmt ? GET_RMT_TITLE(rmt) : "UNKNOWN");
				break;
			}
			case QG_TRIGGER: {
				trig_data *trig = real_trigger(giver->vnum);
				strcpy(buf, trig ? GET_TRIG_NAME(trig) : "UNKNOWN");
				break;
			}
			default: {
				strcpy(buf, "UNKNOWN");
				break;
			}
		}
		
		sprintf(save_buffer + strlen(save_buffer), "%2d. %s [%d] %s\r\n", ++count, quest_giver_types[giver->type], giver->vnum, buf);
	}
	
	// empty list not shown
}


/**
* Gets the display for a set of quest rewards.
*
* @param struct quest_reward *list Pointer to the start of a list of quest rewards.
* @param char *save_buffer A buffer to store the result to.
*/
void get_quest_reward_display(struct quest_reward *list, char *save_buffer) {
	char buf[MAX_STRING_LENGTH];
	struct quest_reward *reward;
	int count = 0;
	
	*save_buffer = '\0';
	LL_FOREACH(list, reward) {
		// QR_x
		switch (reward->type) {
			case QR_BONUS_EXP: {
				strcpy(buf, "%d exp");
				break;
			}
			case QR_COINS: {
				sprintf(buf, "%d %s coin%s", reward->amount, reward->vnum == OTHER_COIN ? "misc" : "empire", PLURAL(reward->amount));
				break;
			}
			case QR_OBJECT: {
				sprintf(buf, "[%d] %dx %s", reward->vnum, reward->amount, get_obj_name_by_proto(reward->vnum));
				break;
			}
			case QR_SET_SKILL: {
				sprintf(buf, "[%d] %d %s", reward->vnum, reward->amount, get_skill_name_by_vnum(reward->vnum));
				break;
			}
			case QR_SKILL_EXP: {
				sprintf(buf, "[%d] %d%% %s", reward->vnum, reward->amount, get_skill_name_by_vnum(reward->vnum));
				break;
			}
			case QR_SKILL_LEVELS: {
				sprintf(buf, "[%d] %dx %s", reward->vnum, reward->amount, get_skill_name_by_vnum(reward->vnum));
				break;
			}
			default: {
				sprintf(buf, "%dx UNKNOWN", reward->amount);
				break;
			}
		}
		
		sprintf(save_buffer + strlen(save_buffer), "%2d. %s: %s\r\n", ++count, quest_reward_types[reward->type], buf);
	}
	
	// empty list not shown
}


/**
* Gets the display for a set of quest tasks.
*
* @param struct quest_task *list Pointer to the start of a list of quest tasks.
* @param char *save_buffer A buffer to store the result to.
*/
void get_quest_task_display(struct quest_task *list, char *save_buffer) {
	char buf[MAX_STRING_LENGTH], lbuf[MAX_STRING_LENGTH];
	struct quest_task *task;
	int count = 0;
	
	*save_buffer = '\0';
	LL_FOREACH(list, task) {
		// QT_x
		switch (task->type) {
			case QT_COMPLETED_QUEST: {
				sprintf(buf, "[%d] %s", task->vnum, get_quest_name_by_proto(task->vnum));
				break;
			}
			case QT_GET_COMPONENT: {
				if (task->misc) {
					prettier_sprintbit(task->misc, component_flags, lbuf);
					strcat(lbuf, " ");
				}
				else {
					*lbuf = '\0';
				}
				sprintf(buf, "%dx (%s%s)", task->needed, lbuf, component_types[task->vnum]);
				break;
			}
			case QT_GET_OBJECT: {
				sprintf(buf, "[%d] %dx %s", task->vnum, task->needed, get_obj_name_by_proto(task->vnum));
				break;
			}
			case QT_KILL_MOB: {
				sprintf(buf, "[%d] %dx %s", task->vnum, task->needed, get_mob_name_by_proto(task->vnum));
				break;
			}
			case QT_KILL_MOB_FLAGGED: {
				sprintbit(task->misc, action_bits, lbuf, TRUE);
				sprintf(buf, "%dx %s", task->vnum, lbuf);
				break;
			}
			case QT_NOT_COMPLETED_QUEST: {
				sprintf(buf, "[%d] %s", task->vnum, get_quest_name_by_proto(task->vnum));
				break;
			}
			case QT_NOT_ON_QUEST: {
				sprintf(buf, "[%d] %s", task->vnum, get_quest_name_by_proto(task->vnum));
				break;
			}
			case QT_OWN_BUILDING: {
				bld_data *bld = building_proto(task->vnum);
				sprintf(buf, "[%d] %dx %s", task->vnum, task->needed, bld ? GET_BLD_NAME(bld) : "UNKNOWN");
				break;
			}
			case QT_OWN_VEHICLE: {
				sprintf(buf, "[%d] %dx %s", task->vnum, task->needed, get_vehicle_name_by_proto(task->vnum));
				break;
			}
			case QT_SKILL_LEVEL_OVER: {
				sprintf(buf, "[%d] >= %d %s", task->vnum, task->needed, get_skill_name_by_vnum(task->vnum));
				break;
			}
			case QT_SKILL_LEVEL_UNDER: {
				sprintf(buf, "[%d] <= %d %s", task->vnum, task->needed, get_skill_name_by_vnum(task->vnum));
				break;
			}
			case QT_TRIGGERED: {
				strcpy(buf, "unknown");
				break;
			}
			case QT_VISIT_BUILDING: {
				bld_data *bld = building_proto(task->vnum);
				sprintf(buf, "[%d] %s", task->vnum, bld ? GET_BLD_NAME(bld) : "UNKNOWN");
				break;
			}
			case QT_VISIT_ROOM_TEMPLATE: {
				room_template *rmt = room_template_proto(task->vnum);
				sprintf(buf, "[%d] %s", task->vnum, rmt ? GET_RMT_TITLE(rmt) : "UNKNOWN");
				break;
			}
			case QT_VISIT_SECTOR: {
				sector_data *sect = sector_proto(task->vnum);
				sprintf(buf, "[%d] %s", task->vnum, sect ? GET_SECT_NAME(sect) : "UNKNOWN");
				break;
			}
			default: {
				sprintf(buf, "UNKNOWN");
				break;
			}
		}
		
		sprintf(save_buffer + strlen(save_buffer), "%2d. %s: %s\r\n", ++count, quest_tracker_types[task->type], buf);
	}
	
	// empty list not shown
}

/**
* For vstat.
*
* @param char_data *ch The player requesting stats.
* @param quest_data *quest The quest to display.
*/
void do_stat_quest(char_data *ch, quest_data *quest) {
	char buf[MAX_STRING_LENGTH], part[MAX_STRING_LENGTH];
	size_t size;
	
	if (!quest) {
		return;
	}
	
	// first line
	size = snprintf(buf, sizeof(buf), "VNum: [\tc%d\t0], Name: \tc%s\t0\r\n", QUEST_VNUM(quest), QUEST_NAME(quest));
	size += snprintf(buf + size, sizeof(buf) - size, "%s", QUEST_DESCRIPTION(quest));
	size += snprintf(buf + size, sizeof(buf) - size, "-------------------------------------------------\r\n");
	size += snprintf(buf + size, sizeof(buf) - size, "%s", QUEST_COMPLETE_MSG(quest));
	
	sprintbit(QUEST_FLAGS(quest), quest_flags, part, TRUE);
	size += snprintf(buf + size, sizeof(buf) - size, "Flags: \tg%s\t0\r\n", part);
	
	if (QUEST_REPEATABLE_AFTER(quest) == NOT_REPEATABLE) {
		strcpy(part, "never");
	}
	else if (QUEST_REPEATABLE_AFTER(quest) == 0) {
		strcpy(part, "immediate");
	}
	else {
		sprintf(part, "%d minutes (%d:%02d:%02d)", QUEST_REPEATABLE_AFTER(quest), (QUEST_REPEATABLE_AFTER(quest) / (60 * 24)), ((QUEST_REPEATABLE_AFTER(quest) % (60 * 24)) / 60), ((QUEST_REPEATABLE_AFTER(quest) % (60 * 24)) % 60));
	}
	size += snprintf(buf + size, sizeof(buf) - size, "Level limits: [\tc%s\t0], Repatable: [\tc%s\t0]\r\n", level_range_string(QUEST_MIN_LEVEL(quest), QUEST_MAX_LEVEL(quest), 0), part);
		
	get_quest_task_display(QUEST_PREREQS(quest), part);
	size += snprintf(buf + size, sizeof(buf) - size, "Pre-requisites:\r\n%s", *part ? part : " none\r\n");
	
	get_quest_giver_display(QUEST_STARTS_AT(quest), part);
	size += snprintf(buf + size, sizeof(buf) - size, "Starts at:\r\n%s", *part ? part : " nowhere\r\n");
	
	get_quest_giver_display(QUEST_ENDS_AT(quest), part);
	size += snprintf(buf + size, sizeof(buf) - size, "Ends at:\r\n%s", *part ? part : " nowhere\r\n");
	
	get_quest_task_display(QUEST_TASKS(quest), part);
	size += snprintf(buf + size, sizeof(buf) - size, "Tasks:\r\n%s", *part ? part : " none\r\n");
	
	get_quest_reward_display(QUEST_REWARDS(quest), part);
	size += snprintf(buf + size, sizeof(buf) - size, "Rewards:\r\n%s", *part ? part : " none\r\n");
	
	// scripts
	get_script_display(QUEST_SCRIPTS(quest), part);
	size += snprintf(buf + size, sizeof(buf) - size, "Scripts:\r\n%s", QUEST_SCRIPTS(quest) ? part : " none\r\n");
	
	page_string(ch->desc, buf, TRUE);
}


/**
* This is the main recipe display for quest OLC. It displays the user's
* currently-edited quest.
*
* @param char_data *ch The person who is editing a quest and will see its display.
*/
void olc_show_quest(char_data *ch) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	char buf[MAX_STRING_LENGTH], lbuf[MAX_STRING_LENGTH];
	
	if (!quest) {
		return;
	}
	
	*buf = '\0';
	
	sprintf(buf + strlen(buf), "[\tc%d\t0] \tc%s\t0\r\n", GET_OLC_VNUM(ch->desc), !quest_proto(QUEST_VNUM(quest)) ? "new quest" : get_quest_name_by_proto(QUEST_VNUM(quest)));
	sprintf(buf + strlen(buf), "<\tyname\t0> %s\r\n", NULLSAFE(QUEST_NAME(quest)));
	sprintf(buf + strlen(buf), "<\tydescription\t0>\r\n%s", NULLSAFE(QUEST_DESCRIPTION(quest)));
	sprintf(buf + strlen(buf), "<\tycompletemessage\t0>\r\n%s", NULLSAFE(QUEST_COMPLETE_MSG(quest)));
	
	sprintbit(QUEST_FLAGS(quest), quest_flags, lbuf, TRUE);
	sprintf(buf + strlen(buf), "<\tyflags\t0> %s\r\n", lbuf);
	
	if (QUEST_MIN_LEVEL(quest) > 0) {
		sprintf(buf + strlen(buf), "<\tyminlevel\t0> %d\r\n", QUEST_MIN_LEVEL(quest));
	}
	else {
		sprintf(buf + strlen(buf), "<\tyminlevel\t0> none\r\n");
	}
	if (QUEST_MAX_LEVEL(quest) > 0) {
		sprintf(buf + strlen(buf), "<\tymaxlevel\t0> %d\r\n", QUEST_MAX_LEVEL(quest));
	}
	else {
		sprintf(buf + strlen(buf), "<\tymaxlevel\t0> none\r\n");
	}
	
	get_quest_task_display(QUEST_PREREQS(quest), lbuf);
	sprintf(buf + strlen(buf), "Pre-requisites: <\typrereqs\t0>\r\n%s", lbuf);
	
	if (QUEST_REPEATABLE_AFTER(quest) == NOT_REPEATABLE) {
		sprintf(buf + strlen(buf), "<\tyrepeat\t0> never\r\n");
	}
	else if (QUEST_REPEATABLE_AFTER(quest) > 0) {
		sprintf(buf + strlen(buf), "<\tyrepeat\t0> %d minutes (%d:%02d:%02d)", QUEST_REPEATABLE_AFTER(quest), (QUEST_REPEATABLE_AFTER(quest) / (60 * 24)), ((QUEST_REPEATABLE_AFTER(quest) % (60 * 24)) / 60), ((QUEST_REPEATABLE_AFTER(quest) % (60 * 24)) % 60));
	}
	else if (QUEST_REPEATABLE_AFTER(quest) == 0) {
		sprintf(buf + strlen(buf), "<\tyrepeat\t0> immediately\r\n");
	}
	
	get_quest_giver_display(QUEST_STARTS_AT(quest), lbuf);
	sprintf(buf + strlen(buf), "Starts at: <\tystarts\t0>\r\n%s", lbuf);
	
	get_quest_giver_display(QUEST_ENDS_AT(quest), lbuf);
	sprintf(buf + strlen(buf), "Ends at: <\tyends\t0>\r\n%s", lbuf);
	
	get_quest_task_display(QUEST_TASKS(quest), lbuf);
	sprintf(buf + strlen(buf), "Tasks: <\tytasks\t0>\r\n%s", lbuf);
	
	get_quest_reward_display(QUEST_REWARDS(quest), lbuf);
	sprintf(buf + strlen(buf), "Rewards: <\tyrewards\t0>\r\n%s", lbuf);
	
	// scripts
	sprintf(buf + strlen(buf), "Scripts: <\tyscript\t0>\r\n");
	if (QUEST_SCRIPTS(quest)) {
		get_script_display(QUEST_SCRIPTS(quest), lbuf);
		strcat(buf, lbuf);
	}
	
	page_string(ch->desc, buf, TRUE);
}


/**
* Searches the quest db for a match, and prints it to the character.
*
* @param char *searchname The search string.
* @param char_data *ch The player who is searching.
* @return int The number of matches shown.
*/
int vnum_quest(char *searchname, char_data *ch) {
	quest_data *iter, *next_iter;
	int found = 0;
	
	HASH_ITER(hh, quest_table, iter, next_iter) {
		if (multi_isname(searchname, QUEST_NAME(iter))) {
			msg_to_char(ch, "%3d. [%5d] %s\r\n", ++found, QUEST_VNUM(iter), QUEST_NAME(iter));
		}
	}
	
	return found;
}


 //////////////////////////////////////////////////////////////////////////////
//// OLC MODULES /////////////////////////////////////////////////////////////

OLC_MODULE(qedit_completemessage) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	
	if (ch->desc->str) {
		msg_to_char(ch, "You are already editing a string.\r\n");
	}
	else {
		sprintf(buf, "completion message for %s", QUEST_NAME(quest));
		start_string_editor(ch->desc, buf, &QUEST_COMPLETE_MSG(quest), MAX_ITEM_DESCRIPTION);
	}
}

OLC_MODULE(qedit_description) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	
	if (ch->desc->str) {
		msg_to_char(ch, "You are already editing a string.\r\n");
	}
	else {
		sprintf(buf, "description for %s", QUEST_NAME(quest));
		start_string_editor(ch->desc, buf, &QUEST_DESCRIPTION(quest), MAX_ITEM_DESCRIPTION);
	}
}


OLC_MODULE(qedit_flags) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	bool had_indev = IS_SET(QUEST_FLAGS(quest), QST_IN_DEVELOPMENT) ? TRUE : FALSE;
	
	QUEST_FLAGS(quest) = olc_process_flag(ch, argument, "quest", "flags", quest_flags, QUEST_FLAGS(quest));
	
	// validate removal of IN-DEVELOPMENT
	if (had_indev && !IS_SET(QUEST_FLAGS(quest), QST_IN_DEVELOPMENT) && GET_ACCESS_LEVEL(ch) < LVL_UNRESTRICTED_BUILDER && !OLC_FLAGGED(ch, OLC_FLAG_CLEAR_IN_DEV)) {
		msg_to_char(ch, "You don't have permission to remove the IN-DEVELOPMENT flag.\r\n");
		SET_BIT(QUEST_FLAGS(quest), QST_IN_DEVELOPMENT);
	}
}


OLC_MODULE(qedit_name) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	olc_process_string(ch, argument, "name", &QUEST_NAME(quest));
}


OLC_MODULE(qedit_maxlevel) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	QUEST_MAX_LEVEL(quest) = olc_process_number(ch, argument, "maximum level", "maxlevel", 0, MAX_INT, QUEST_MAX_LEVEL(quest));
}


OLC_MODULE(qedit_minlevel) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	QUEST_MIN_LEVEL(quest) = olc_process_number(ch, argument, "minimum level", "minlevel", 0, MAX_INT, QUEST_MIN_LEVEL(quest));
}


OLC_MODULE(qedit_repeat) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	
	if (is_abbrev(argument, "never") || is_abbrev(argument, "none")) {
		QUEST_REPEATABLE_AFTER(quest) = NOT_REPEATABLE;
		msg_to_char(ch, "It is now non-repeatable.\r\n");
	}
	else if (is_abbrev(argument, "immediately")) {
		QUEST_REPEATABLE_AFTER(quest) = 0;
		msg_to_char(ch, "It is now immediately repeatable.\r\n");
	}
	else {
		QUEST_REPEATABLE_AFTER(quest) = olc_process_number(ch, argument, "repeatable after", "repeat", 0, MAX_INT, QUEST_REPEATABLE_AFTER(quest));
		msg_to_char(ch, "It now repeats after %d minutes (%d:%02d:%02d).\r\n", QUEST_REPEATABLE_AFTER(quest), (QUEST_REPEATABLE_AFTER(quest) / (60 * 24)), ((QUEST_REPEATABLE_AFTER(quest) % (60 * 24)) / 60), ((QUEST_REPEATABLE_AFTER(quest) % (60 * 24)) % 60));
	}
}


OLC_MODULE(qedit_script) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	olc_process_script(ch, argument, &QUEST_SCRIPTS(quest), WLD_TRIGGER);
}
