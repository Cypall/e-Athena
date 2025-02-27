// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/cbasetypes.h"
#include "../common/core.h" // get_svn_revision()
#include "../common/malloc.h"
#include "../common/nullpo.h"
#include "../common/showmsg.h"
#include "../common/socket.h" // session[]
#include "../common/strlib.h" // safestrncpy()
#include "../common/timer.h"
#include "../common/utils.h"
#include "../common/mmo.h" //NAME_LENGTH

#include "atcommand.h" // get_atcommand_level()
#include "battle.h" // battle_config
#include "battleground.h"
#include "chrif.h"
#include "clif.h"
#include "date.h" // is_day_of_*()
#include "intif.h"
#include "itemdb.h"
#include "log.h"
#include "mail.h"
#include "map.h"
#include "path.h"
#include "homunculus.h"
#include "instance.h"
#include "mercenary.h"
#include "mob.h" // MAX_MOB_RACE_DB
#include "npc.h" // fake_nd
#include "pet.h" // pet_unlocktarget()
#include "party.h" // party_search()
#include "guild.h" // guild_search(), guild_request_info()
#include "script.h" // script_config
#include "skill.h"
#include "status.h" // struct status_data
#include "pc.h"
#include "quest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define PVP_CALCRANK_INTERVAL 1000	// PVPÊvZÌÔu
static unsigned int exp_table[CLASS_COUNT][2][MAX_LEVEL];
static unsigned int max_level[CLASS_COUNT][2];
static unsigned int statp[MAX_LEVEL+1];

// h-files are for declarations, not for implementations... [Shinomori]
struct skill_tree_entry skill_tree[CLASS_COUNT][MAX_SKILL_TREE];
// timer for night.day implementation
int day_timer_tid;
int night_timer_tid;

struct fame_list smith_fame_list[MAX_FAME_LIST];
struct fame_list chemist_fame_list[MAX_FAME_LIST];
struct fame_list taekwon_fame_list[MAX_FAME_LIST];

static unsigned short equip_pos[EQI_MAX]={EQP_ACC_L,EQP_ACC_R,EQP_SHOES,EQP_GARMENT,EQP_HEAD_LOW,EQP_HEAD_MID,EQP_HEAD_TOP,EQP_ARMOR,EQP_HAND_L,EQP_HAND_R,EQP_AMMO};

#define MOTD_LINE_SIZE 128
char motd_text[MOTD_LINE_SIZE][256]; // Message of the day buffer [Valaris]

struct duel duel_list[MAX_DUEL];
int duel_count = 0;

//Links related info to the sd->hate_mob[]/sd->feel_map[] entries
const struct sg_data sg_info[3] = {
		{ SG_SUN_ANGER, SG_SUN_BLESS, SG_SUN_COMFORT, "PC_FEEL_SUN", "PC_HATE_MOB_SUN", is_day_of_sun },
		{ SG_MOON_ANGER, SG_MOON_BLESS, SG_MOON_COMFORT, "PC_FEEL_MOON", "PC_HATE_MOB_MOON", is_day_of_moon },
		{ SG_STAR_ANGER, SG_STAR_BLESS, SG_STAR_COMFORT, "PC_FEEL_STAR", "PC_HATE_MOB_STAR", is_day_of_star }
	};

//Converts a class to its array index for CLASS_COUNT defined arrays.
//Note that it does not do a validity check for speed purposes, where parsing
//player input make sure to use a pcdb_checkid first!
int pc_class2idx(int class_) {
	if (class_ >= JOB_NOVICE_HIGH)
		return class_- JOB_NOVICE_HIGH+JOB_MAX_BASIC;
	return class_;
}

int pc_isGM(struct map_session_data* sd)
{
	return sd->gmlevel;
}

static int pc_invincible_timer(int tid, unsigned int tick, int id, intptr data)
{
	struct map_session_data *sd;

	if( (sd=(struct map_session_data *)map_id2sd(id)) == NULL || sd->bl.type!=BL_PC )
		return 1;

	if(sd->invincible_timer != tid){
		ShowError("invincible_timer %d != %d\n",sd->invincible_timer,tid);
		return 0;
	}
	sd->invincible_timer = INVALID_TIMER;
	skill_unit_move(&sd->bl,tick,1);

	return 0;
}

void pc_setinvincibletimer(struct map_session_data* sd, int val) 
{
	nullpo_retv(sd);

	if( sd->invincible_timer != INVALID_TIMER )
		delete_timer(sd->invincible_timer,pc_invincible_timer);
	sd->invincible_timer = add_timer(gettick()+val,pc_invincible_timer,sd->bl.id,0);
}

void pc_delinvincibletimer(struct map_session_data* sd)
{
	nullpo_retv(sd);

	if( sd->invincible_timer != INVALID_TIMER )
	{
		delete_timer(sd->invincible_timer,pc_invincible_timer);
		sd->invincible_timer = INVALID_TIMER;
		skill_unit_move(&sd->bl,gettick(),1);
	}
}

static int pc_spiritball_timer(int tid, unsigned int tick, int id, intptr data)
{
	struct map_session_data *sd;
	int i;

	if( (sd=(struct map_session_data *)map_id2sd(id)) == NULL || sd->bl.type!=BL_PC )
		return 1;

	if( sd->spiritball <= 0 )
	{
		ShowError("pc_spiritball_timer: %d spiritball's available. (aid=%d cid=%d tid=%d)\n", sd->spiritball, sd->status.account_id, sd->status.char_id, tid);
		sd->spiritball = 0;
		return 0;
	}

	ARR_FIND(0, sd->spiritball, i, sd->spirit_timer[i] == tid);
	if( i == sd->spiritball )
	{
		ShowError("pc_spiritball_timer: timer not found (aid=%d cid=%d tid=%d)\n", sd->status.account_id, sd->status.char_id, tid);
		return 0;
	}

	sd->spiritball--;
	if( i != sd->spiritball )
		memmove(sd->spirit_timer+i, sd->spirit_timer+i+1, (sd->spiritball-i)*sizeof(int));
	sd->spirit_timer[sd->spiritball] = INVALID_TIMER;

	clif_spiritball(sd);

	return 0;
}

int pc_addspiritball(struct map_session_data *sd,int interval,int max)
{
	int tid, i;

	nullpo_retr(0, sd);

	if(max > MAX_SKILL_LEVEL)
		max = MAX_SKILL_LEVEL;
	if(sd->spiritball < 0)
		sd->spiritball = 0;

	if( sd->spiritball && sd->spiritball >= max )
	{
		if(sd->spirit_timer[0] != -1)
			delete_timer(sd->spirit_timer[0],pc_spiritball_timer);
		sd->spiritball--;
		if( sd->spiritball != 0 )
			memmove(sd->spirit_timer+0, sd->spirit_timer+1, (sd->spiritball)*sizeof(int));
		sd->spirit_timer[sd->spiritball] = INVALID_TIMER;
	}

	tid = add_timer(gettick()+interval, pc_spiritball_timer, sd->bl.id, 0);
	ARR_FIND(0, sd->spiritball, i, sd->spirit_timer[i] == INVALID_TIMER || DIFF_TICK(get_timer(tid)->tick, get_timer(sd->spirit_timer[i])->tick) < 0);
	if( i != sd->spiritball )
		memmove(sd->spirit_timer+i+1, sd->spirit_timer+i, (sd->spiritball-i)*sizeof(int));
	sd->spirit_timer[i] = tid;
	sd->spiritball++;
	clif_spiritball(sd);

	return 0;
}

int pc_delspiritball(struct map_session_data *sd,int count,int type)
{
	int i;

	nullpo_retr(0, sd);

	if(sd->spiritball <= 0) {
		sd->spiritball = 0;
		return 0;
	}

	if(count <= 0)
		return 0;
	if(count > sd->spiritball)
		count = sd->spiritball;
	sd->spiritball -= count;
	if(count > MAX_SKILL_LEVEL)
		count = MAX_SKILL_LEVEL;

	for(i=0;i<count;i++) {
		if(sd->spirit_timer[i] != -1) {
			delete_timer(sd->spirit_timer[i],pc_spiritball_timer);
			sd->spirit_timer[i] = -1;
		}
	}
	for(i=count;i<MAX_SKILL_LEVEL;i++) {
		sd->spirit_timer[i-count] = sd->spirit_timer[i];
		sd->spirit_timer[i] = -1;
	}

	if(!type)
		clif_spiritball(sd);

	return 0;
}

// Increases a player's fame points and displays a notice to him
void pc_addfame(struct map_session_data *sd,int count)
{
	nullpo_retv(sd);
	sd->status.fame += count;
	if(sd->status.fame > MAX_FAME)
		sd->status.fame = MAX_FAME;
	switch(sd->class_&MAPID_UPPERMASK){
		case MAPID_BLACKSMITH: // Blacksmith
			clif_fame_blacksmith(sd,count);
			break;
		case MAPID_ALCHEMIST: // Alchemist
			clif_fame_alchemist(sd,count);
			break;
		case MAPID_TAEKWON: // Taekwon
			clif_fame_taekwon(sd,count);
			break;	
	}
	chrif_updatefamelist(sd);
}

// Check whether a player ID is in the fame rankers' list of its job, returns his/her position if so, 0 else
unsigned char pc_famerank(int char_id, int job)
{
	int i;
	
	switch(job){
		case MAPID_BLACKSMITH: // Blacksmith
		    for(i = 0; i < MAX_FAME_LIST; i++){
				if(smith_fame_list[i].id == char_id)
				    return i + 1;
			}
			break;
		case MAPID_ALCHEMIST: // Alchemist
			for(i = 0; i < MAX_FAME_LIST; i++){
				if(chemist_fame_list[i].id == char_id)
					return i + 1;
			}
			break;
		case MAPID_TAEKWON: // Taekwon
			for(i = 0; i < MAX_FAME_LIST; i++){
				if(taekwon_fame_list[i].id == char_id)
					return i + 1;
			}
			break;
	}

	return 0;
}

int pc_setrestartvalue(struct map_session_data *sd,int type)
{
	struct status_data *status, *b_status;
	nullpo_retr(0, sd);

	b_status = &sd->base_status;
	status = &sd->battle_status;

	if (type&1)
	{	//Normal resurrection
		status->hp = 1; //Otherwise status_heal may fail if dead.
		status_heal(&sd->bl, b_status->hp, b_status->sp>status->sp?b_status->sp-status->sp:0, 1);
	} else { //Just for saving on the char-server (with values as if respawned)
		sd->status.hp = b_status->hp;
		sd->status.sp = (status->sp < b_status->sp)?b_status->sp:status->sp;
	}
	return 0;
}

/*==========================================
	Rental System
 *------------------------------------------*/
static int pc_inventory_rental_end(int tid, unsigned int tick, int id, intptr data)
{
	struct map_session_data *sd = map_id2sd(id);
	if( sd == NULL )
		return 0;
	if( tid != sd->rental_timer )
	{
		ShowError("pc_inventory_rental_end: invalid timer id.\n");
		return 0;
	}

	pc_inventory_rentals(sd);
	return 1;
}

int pc_inventory_rental_clear(struct map_session_data *sd)
{
	if( sd->rental_timer != INVALID_TIMER )
	{
		delete_timer(sd->rental_timer, pc_inventory_rental_end);
		sd->rental_timer = -1;
	}

	return 1;
}

void pc_inventory_rentals(struct map_session_data *sd)
{
	int i, c = 0;
	unsigned int expire_tick, next_tick = UINT_MAX;

	for( i = 0; i < MAX_INVENTORY; i++ )
	{ // Check for Rentals on Inventory
		if( sd->status.inventory[i].nameid == 0 )
			continue; // Nothing here
		if( sd->status.inventory[i].expire_time == 0 )
			continue;

		if( sd->status.inventory[i].expire_time <= time(NULL) )
		{
			clif_rental_expired(sd->fd, sd->status.inventory[i].nameid);
			pc_delitem(sd, i, sd->status.inventory[i].amount, 0);
		}
		else
		{
			expire_tick = (unsigned int)(sd->status.inventory[i].expire_time - time(NULL)) * 1000;
			clif_rental_time(sd->fd, sd->status.inventory[i].nameid, (int)(expire_tick / 1000));
			next_tick = min(expire_tick, next_tick);
			c++;
		}
	}

	for( i = 0; i < MAX_CART; i++ )
	{ // Check for Rentals on Cart
		if( sd->status.cart[i].nameid == 0 )
			continue; // Nothing here
		if( sd->status.cart[i].expire_time == 0 )
			continue;

		if( sd->status.cart[i].expire_time <= time(NULL) )
		{
			clif_rental_expired(sd->fd, sd->status.cart[i].nameid);
			pc_cart_delitem(sd, i, 1, 0);
		}
		else
		{
			expire_tick = (unsigned int)(sd->status.cart[i].expire_time - time(NULL)) * 1000;
			clif_rental_time(sd->fd, sd->status.cart[i].nameid, (int)(expire_tick / 1000));
			next_tick = min(expire_tick, next_tick);
			c++;
		}
	}

	for( i = 0; i < MAX_STORAGE; i++ )
	{ // Check for Rentals on Storage
		if( sd->status.storage.items[i].nameid == 0 )
			continue;
		if( sd->status.storage.items[i].expire_time == 0 )
			continue;

		if( sd->status.storage.items[i].expire_time <= time(NULL) )
		{
			clif_rental_expired(sd->fd, sd->status.storage.items[i].nameid);
			storage_delitem(sd, i, 1);
		}
		else
		{
			expire_tick = (unsigned int)(sd->status.storage.items[i].expire_time - time(NULL)) * 1000;
			clif_rental_time(sd->fd, sd->status.storage.items[i].nameid, (int)(expire_tick / 1000));
			next_tick = min(expire_tick, next_tick);
			c++;
		}
	}

	if( c > 0 ) // min(next_tick,3600000) 1 hour each timer to keep announcing to the owner, and to avoid a but with rental time > 15 days
		sd->rental_timer = add_timer(gettick() + min(next_tick,3600000), pc_inventory_rental_end, sd->bl.id, 0);
	else
		sd->rental_timer = INVALID_TIMER;
}

void pc_inventory_rental_add(struct map_session_data *sd, int seconds)
{
	const struct TimerData * td;
	int tick = seconds * 1000;

	if( sd == NULL )
		return;

	if( sd->rental_timer != INVALID_TIMER )
	{
		td = get_timer(sd->rental_timer);
		if( DIFF_TICK(td->tick, gettick()) > tick )
		{ // Update Timer as this one ends first than the current one
			pc_inventory_rental_clear(sd);
			sd->rental_timer = add_timer(gettick() + tick, pc_inventory_rental_end, sd->bl.id, 0);
		}
	}
	else
		sd->rental_timer = add_timer(gettick() + min(tick,3600000), pc_inventory_rental_end, sd->bl.id, 0);
}

/*==========================================
	Determines if the GM can give / drop / trade / vend items
    Args: GM Level (current player GM level)
 *------------------------------------------*/
bool pc_can_give_items(int level)
{
	return( level < battle_config.gm_cant_drop_min_lv || level > battle_config.gm_cant_drop_max_lv );
}

/*==========================================
 * prepares character for saving.
 *------------------------------------------*/
int pc_makesavestatus(struct map_session_data *sd)
{
	nullpo_retr(0, sd);

	if(!battle_config.save_clothcolor)
		sd->status.clothes_color=0;

  	//Only copy the Cart/Peco/Falcon options, the rest are handled via 
	//status change load/saving. [Skotlex]
	sd->status.option = sd->sc.option&(OPTION_CART|OPTION_FALCON|OPTION_RIDING);
		
	if (sd->sc.data[SC_JAILED])
	{	//When Jailed, do not move last point.
		if(pc_isdead(sd)){
			pc_setrestartvalue(sd,0);
		} else {
			sd->status.hp = sd->battle_status.hp;
			sd->status.sp = sd->battle_status.sp;
		}
		sd->status.last_point.map = sd->mapindex;
		sd->status.last_point.x = sd->bl.x;
		sd->status.last_point.y = sd->bl.y;
		return 0;
	}

	if(pc_isdead(sd)){
		pc_setrestartvalue(sd,0);
		memcpy(&sd->status.last_point,&sd->status.save_point,sizeof(sd->status.last_point));
	} else {
		sd->status.hp = sd->battle_status.hp;
		sd->status.sp = sd->battle_status.sp;
		sd->status.last_point.map = sd->mapindex;
		sd->status.last_point.x = sd->bl.x;
		sd->status.last_point.y = sd->bl.y;
	}

	if(map[sd->bl.m].flag.nosave){
		struct map_data *m=&map[sd->bl.m];
		if(m->save.map)
			memcpy(&sd->status.last_point,&m->save,sizeof(sd->status.last_point));
		else
			memcpy(&sd->status.last_point,&sd->status.save_point,sizeof(sd->status.last_point));
	}

	return 0;
}

/*==========================================
 * Ú?bÌú?
 *------------------------------------------*/
int pc_setnewpc(struct map_session_data *sd, int account_id, int char_id, int login_id1, unsigned int client_tick, int sex, int fd)
{
	nullpo_retr(0, sd);

	sd->bl.id        = account_id;
	sd->status.account_id   = account_id;
	sd->status.char_id      = char_id;
	sd->status.sex   = sex;
	sd->login_id1    = login_id1;
	sd->login_id2    = 0; // at this point, we can not know the value :(
	sd->client_tick  = client_tick;
	sd->state.active = 0; //to be set to 1 after player is fully authed and loaded.
	sd->bl.type      = BL_PC;
	sd->canlog_tick  = gettick();
	//Required to prevent homunculus copuing a base speed of 0.
	sd->battle_status.speed = sd->base_status.speed = DEFAULT_WALK_SPEED;
	return 0;
}

int pc_equippoint(struct map_session_data *sd,int n)
{
	int ep = 0;

	nullpo_retr(0, sd);

	if(!sd->inventory_data[n])
		return 0;

	if (!itemdb_isequip2(sd->inventory_data[n]))
		return 0; //Not equippable by players.
	
	ep = sd->inventory_data[n]->equip;
	if(sd->inventory_data[n]->look == W_DAGGER	||
		sd->inventory_data[n]->look == W_1HSWORD ||
		sd->inventory_data[n]->look == W_1HAXE) {
		if(ep == EQP_HAND_R && (pc_checkskill(sd,AS_LEFT) > 0 || (sd->class_&MAPID_UPPERMASK) == MAPID_ASSASSIN))
			return EQP_ARMS;
	}
	return ep;
}

int pc_setinventorydata(struct map_session_data *sd)
{
	int i,id;

	nullpo_retr(0, sd);

	for(i=0;i<MAX_INVENTORY;i++) {
		id = sd->status.inventory[i].nameid;
		sd->inventory_data[i] = id?itemdb_search(id):NULL;
	}
	return 0;
}

int pc_calcweapontype(struct map_session_data *sd)
{
	nullpo_retr(0, sd);

	// single-hand
	if(sd->weapontype2 == W_FIST) {
		sd->status.weapon = sd->weapontype1;
		return 1;
	}
	if(sd->weapontype1 == W_FIST) {
		sd->status.weapon = sd->weapontype2;
		return 1;
	}
	// dual-wield
	sd->status.weapon = 0;
	switch (sd->weapontype1){
	case W_DAGGER:
		switch (sd->weapontype2) {
		case W_DAGGER:  sd->status.weapon = W_DOUBLE_DD; break;
		case W_1HSWORD: sd->status.weapon = W_DOUBLE_DS; break;
		case W_1HAXE:   sd->status.weapon = W_DOUBLE_DA; break;
		}
		break;
	case W_1HSWORD:
		switch (sd->weapontype2) {
		case W_DAGGER:  sd->status.weapon = W_DOUBLE_DS; break;
		case W_1HSWORD: sd->status.weapon = W_DOUBLE_SS; break;
		case W_1HAXE:   sd->status.weapon = W_DOUBLE_SA; break;
		}
		break;
	case W_1HAXE:
		switch (sd->weapontype2) {
		case W_DAGGER:  sd->status.weapon = W_DOUBLE_DA; break;
		case W_1HSWORD: sd->status.weapon = W_DOUBLE_SA; break;
		case W_1HAXE:   sd->status.weapon = W_DOUBLE_AA; break;
		}
	}
	// unknown, default to right hand type
	if (!sd->status.weapon)
		sd->status.weapon = sd->weapontype1;

	return 2;
}

int pc_setequipindex(struct map_session_data *sd)
{
	int i,j;

	nullpo_retr(0, sd);

	for(i=0;i<EQI_MAX;i++)
		sd->equip_index[i] = -1;

	for(i=0;i<MAX_INVENTORY;i++) {
		if(sd->status.inventory[i].nameid <= 0)
			continue;
		if(sd->status.inventory[i].equip) {
			for(j=0;j<EQI_MAX;j++)
				if(sd->status.inventory[i].equip & equip_pos[j])
					sd->equip_index[j] = i;

			if(sd->status.inventory[i].equip & EQP_HAND_R)
			{
				if(sd->inventory_data[i])
					sd->weapontype1 = sd->inventory_data[i]->look;
				else
					sd->weapontype1 = 0;
			}

			if( sd->status.inventory[i].equip & EQP_HAND_L )
			{
				if( sd->inventory_data[i] && sd->inventory_data[i]->type == 4 )
					sd->weapontype2 = sd->inventory_data[i]->look;
				else
					sd->weapontype2 = 0;
			}
		}
	}
	pc_calcweapontype(sd);

	return 0;
}

static int pc_isAllowedCardOn(struct map_session_data *sd,int s,int eqindex,int flag)
{
	int i;
	struct item *item = &sd->status.inventory[eqindex];
	struct item_data *data;
	//Crafted/made/hatched items.
	if (itemdb_isspecial(item->card[0]))
		return 1;
	
	ARR_FIND( 0, s, i, item->card[i] && (data = itemdb_exists(item->card[i])) != NULL && data->flag.no_equip&flag );
	return( i < s ) ? 0 : 1;
}

bool pc_isequipped(struct map_session_data *sd, int nameid)
{
	int i, j, index;

	for( i = 0; i < EQI_MAX; i++ )
	{
		index = sd->equip_index[i];
		if( index < 0 ) continue;

		if( i == EQI_HAND_R && sd->equip_index[EQI_HAND_L] == index ) continue;
		if( i == EQI_HEAD_MID && sd->equip_index[EQI_HEAD_LOW] == index ) continue;
		if( i == EQI_HEAD_TOP && (sd->equip_index[EQI_HEAD_MID] == index || sd->equip_index[EQI_HEAD_LOW] == index) ) continue;
	
		if( !sd->inventory_data[index] ) continue;

		if( sd->inventory_data[index]->nameid == nameid )
			return true;

		for( j = 0; j < sd->inventory_data[index]->slot; j++ )
			if( sd->status.inventory[index].card[j] == nameid )
				return true;
	}

	return false;
}

bool pc_can_Adopt(struct map_session_data *p1_sd, struct map_session_data *p2_sd, struct map_session_data *b_sd )
{
	if( !p1_sd || !p2_sd || !b_sd )
		return false;

	if( b_sd->status.father || b_sd->status.mother || b_sd->adopt_invite )
		return false; // already adopted baby / in adopt request

	if( !p1_sd->status.partner_id || !p1_sd->status.party_id || p1_sd->status.party_id != b_sd->status.party_id )
		return false; // You need to be married and in party with baby to adopt

	if( p1_sd->status.partner_id != p2_sd->status.char_id || p2_sd->status.partner_id != p1_sd->status.char_id )
		return false; // Not married, wrong married

	if( p2_sd->status.party_id != p1_sd->status.party_id )
		return false; // Both parents need to be in the same party

	// Parents need to have their ring equipped
	if( !pc_isequipped(p1_sd, WEDDING_RING_M) && !pc_isequipped(p1_sd, WEDDING_RING_F) )
		return false; 

	if( !pc_isequipped(p2_sd, WEDDING_RING_M) && !pc_isequipped(p2_sd, WEDDING_RING_F) )
		return false;

	// Already adopted a baby
	if( p1_sd->status.child || p2_sd->status.child ) {
		clif_Adopt_reply(p1_sd, 0);
		return false;
	}

	// Parents need at least lvl 70 to adopt
	if( p1_sd->status.base_level < 70 || p2_sd->status.base_level < 70 ) {
		clif_Adopt_reply(p1_sd, 1);
		return false;
	}

	if( b_sd->status.partner_id ) {
		clif_Adopt_reply(p1_sd, 2);
		return false;
	}

	if( !(b_sd->status.class_ >= JOB_NOVICE && b_sd->status.class_ <= JOB_THIEF) )
		return false;

	return true;
}

/*==========================================
 * Adoption Process
 *------------------------------------------*/
bool pc_adoption(struct map_session_data *p1_sd, struct map_session_data *p2_sd, struct map_session_data *b_sd)
{
	int job, joblevel;
	unsigned int jobexp;
	
	if( !pc_can_Adopt(p1_sd, p2_sd, b_sd) )
		return false;

	// Preserve current job levels and progress
	joblevel = b_sd->status.job_level;
	jobexp = b_sd->status.job_exp;

	job = pc_mapid2jobid(b_sd->class_|JOBL_BABY, b_sd->status.sex);
	if( job != -1 && !pc_jobchange(b_sd, job, 0) )
	{ // Success, proceed to configure parents and baby skills
		p1_sd->status.child = b_sd->status.char_id;
		p2_sd->status.child = b_sd->status.char_id;
		b_sd->status.father = p1_sd->status.char_id;
		b_sd->status.mother = p2_sd->status.char_id;

		// Restore progress
		b_sd->status.job_level = joblevel;
		clif_updatestatus(b_sd, SP_JOBLEVEL);
		b_sd->status.job_exp = jobexp;
		clif_updatestatus(b_sd, SP_JOBEXP);

		// Baby Skills
		pc_skill(b_sd, WE_BABY, 1, 0);
		pc_skill(b_sd, WE_CALLPARENT, 1, 0);

		// Parents Skills
		pc_skill(p1_sd, WE_CALLBABY, 1, 0);
		pc_skill(p2_sd, WE_CALLBABY, 1, 0);
		
		return true;
	}

	return false; // Job Change Fail
}

int pc_isequip(struct map_session_data *sd,int n)
{
	struct item_data *item;
	//?¶â{qÌêÌ³ÌEÆðZo·é

	nullpo_retr(0, sd);

	item = sd->inventory_data[n];

	if( battle_config.gm_allequip>0 && pc_isGM(sd)>=battle_config.gm_allequip )
		return 1;

	if(item == NULL)
		return 0;
	if(item->elv && sd->status.base_level < (unsigned int)item->elv)
		return 0;
	if(item->sex != 2 && sd->status.sex != item->sex)
		return 0;
	if(map[sd->bl.m].flag.pvp && ((item->flag.no_equip&1) || !pc_isAllowedCardOn(sd,item->slot,n,1)))
		return 0;
	if(map_flag_gvg(sd->bl.m) && ((item->flag.no_equip&2) || !pc_isAllowedCardOn(sd,item->slot,n,2)))
		return 0; 
	if(map[sd->bl.m].flag.restricted)
	{
		int flag =map[sd->bl.m].zone;
		if (item->flag.no_equip&flag || !pc_isAllowedCardOn(sd,item->slot,n,flag))
			return 0;
	}

	if (sd->sc.count) {
			
		if(item->equip & EQP_ARMS && item->type == IT_WEAPON && sd->sc.data[SC_STRIPWEAPON]) // Also works with left-hand weapons [DracoRPG]
			return 0;
		if(item->equip & EQP_SHIELD && item->type == IT_ARMOR && sd->sc.data[SC_STRIPSHIELD])
			return 0;
		if(item->equip & EQP_ARMOR && sd->sc.data[SC_STRIPARMOR])
			return 0;
		if(item->equip & EQP_HELM && sd->sc.data[SC_STRIPHELM])
			return 0;

		if (sd->sc.data[SC_SPIRIT] && sd->sc.data[SC_SPIRIT]->val2 == SL_SUPERNOVICE) {
			//Spirit of Super Novice equip bonuses. [Skotlex]
			if (sd->status.base_level > 90 && item->equip & EQP_HELM)
				return 1; //Can equip all helms

			if (sd->status.base_level > 96 && item->equip & EQP_ARMS && item->type == IT_WEAPON)
				switch(item->look) { //In weapons, the look determines type of weapon.
					case W_DAGGER: //Level 4 Knives are equippable.. this means all knives, I'd guess?
					case W_1HSWORD: //All 1H swords
					case W_1HAXE: //All 1H Axes
					case W_MACE: //All 1H Maces
					case W_STAFF: //All 1H Staves
						return 1;
				}
		}
	}
	//Not equipable by class. [Skotlex]
	if (!(1<<(sd->class_&MAPID_BASEMASK)&item->class_base[(sd->class_&JOBL_2_1)?1:((sd->class_&JOBL_2_2)?2:0)]))
		return 0;
	
	//Not equipable by upper class. [Skotlex]
	if(!(1<<((sd->class_&JOBL_UPPER)?1:((sd->class_&JOBL_BABY)?2:0))&item->class_upper))
		return 0;

	return 1;
}

/*==========================================
 * session idÉâè³µ
 * charI©ççêÄ«½Xe?^XðÝè
 *------------------------------------------*/
bool pc_authok(struct map_session_data *sd, int login_id2, time_t expiration_time, int gmlevel, struct mmo_charstatus *st)
{
	int i;
	unsigned long tick = gettick();
	uint32 ip = session[sd->fd]->client_addr;

	sd->login_id2 = login_id2;
	sd->gmlevel = gmlevel;
	memcpy(&sd->status, st, sizeof(*st));

	if (st->sex != sd->status.sex) {
		clif_authfail_fd(sd->fd, 0);
		return false;
	}

	//Set the map-server used job id. [Skotlex]
	i = pc_jobid2mapid(sd->status.class_);
	if (i == -1) { //Invalid class?
		ShowError("pc_authok: Invalid class %d for player %s (%d:%d). Class was changed to novice.\n", sd->status.class_, sd->status.name, sd->status.account_id, sd->status.char_id);
		sd->status.class_ = JOB_NOVICE;
		sd->class_ = MAPID_NOVICE;
	} else
		sd->class_ = i; 
	//Initializations to null/0 unneeded since map_session_data was filled with 0 upon allocation.
	if(!sd->status.hp) pc_setdead(sd);
	sd->state.connect_new = 1;

	sd->followtimer = INVALID_TIMER; // [MouseJstr]
	sd->invincible_timer = INVALID_TIMER;
	sd->npc_timer_id = INVALID_TIMER;
	sd->pvp_timer = INVALID_TIMER;
	
	sd->canuseitem_tick = tick;
	sd->canequip_tick = tick;
	sd->cantalk_tick = tick;
	sd->cansendmail_tick = tick;

	for(i = 0; i < MAX_SKILL_LEVEL; i++)
		sd->spirit_timer[i] = INVALID_TIMER;
	for(i = 0; i < ARRAYLENGTH(sd->autobonus); i++)
		sd->autobonus[i].active = INVALID_TIMER;
	for(i = 0; i < ARRAYLENGTH(sd->autobonus2); i++)
		sd->autobonus2[i].active = INVALID_TIMER;
	for(i = 0; i < ARRAYLENGTH(sd->autobonus3); i++)
		sd->autobonus3[i].active = INVALID_TIMER;

	if (battle_config.item_auto_get)
		sd->state.autoloot = 10000;

	if (battle_config.disp_experience)
		sd->state.showexp = 1;
	if (battle_config.disp_zeny)
		sd->state.showzeny = 1;
	
	if (!(battle_config.display_skill_fail&2))
		sd->state.showdelay = 1;
		
	// ACe`FbN
	pc_setinventorydata(sd);
	pc_checkitem(sd);

	status_change_init(&sd->bl);
	if ((battle_config.atc_gmonly == 0 || pc_isGM(sd)) && (pc_isGM(sd) >= get_atcommand_level(atcommand_hide)))
		sd->status.option &= (OPTION_MASK | OPTION_INVISIBLE);
	else
		sd->status.option &= OPTION_MASK;

	sd->sc.option = sd->status.option; //This is the actual option used in battle.
	//Set here because we need the inventory data for weapon sprite parsing.
	status_set_viewdata(&sd->bl, sd->status.class_);
	unit_dataset(&sd->bl);

	sd->guild_x = -1;
	sd->guild_y = -1;

	// Event Timers
	for( i = 0; i < MAX_EVENTTIMER; i++ )
		sd->eventtimer[i] = -1;
	// Rental Timer
	sd->rental_timer = INVALID_TIMER;

	for( i = 0; i < 3; i++ )
		sd->hate_mob[i] = -1;

	// ÊuÌÝè
	if ((i=pc_setpos(sd,sd->status.last_point.map, sd->status.last_point.x, sd->status.last_point.y, 0)) != 0) {
		ShowError ("Last_point_map %s - id %d not found (error code %d)\n", mapindex_id2name(sd->status.last_point.map), sd->status.last_point.map, i);

		// try warping to a default map instead (church graveyard)
		if (pc_setpos(sd, mapindex_name2id(MAP_PRONTERA), 273, 354, 0) != 0) {
			// if we fail again
			clif_authfail_fd(sd->fd, 0);
			return false;
		}
	}

	clif_authok(sd);

	//Prevent S. Novices from getting the no-death bonus just yet. [Skotlex]
	sd->die_counter=-1;

	//display login notice
	if( sd->gmlevel >= battle_config.lowest_gm_level )
		ShowInfo("GM '"CL_WHITE"%s"CL_RESET"' logged in."
			" (AID/CID: '"CL_WHITE"%d/%d"CL_RESET"',"
			" Packet Ver: '"CL_WHITE"%d"CL_RESET"', IP: '"CL_WHITE"%d.%d.%d.%d"CL_RESET"',"
			" GM Level '"CL_WHITE"%d"CL_RESET"').\n",
			sd->status.name, sd->status.account_id, sd->status.char_id,
			sd->packet_ver, CONVIP(ip), sd->gmlevel);
	else
		ShowInfo("'"CL_WHITE"%s"CL_RESET"' logged in."
			" (AID/CID: '"CL_WHITE"%d/%d"CL_RESET"',"
			" Packet Ver: '"CL_WHITE"%d"CL_RESET"', IP: '"CL_WHITE"%d.%d.%d.%d"CL_RESET"').\n",
			sd->status.name, sd->status.account_id, sd->status.char_id,
			sd->packet_ver, CONVIP(ip));
	
	// Send friends list
	clif_friendslist_send(sd);

	if (battle_config.display_version == 1){
		char buf[256];
		sprintf(buf, "eAthena SVN version: %s", get_svn_revision());
		clif_displaymessage(sd->fd, buf);
	}

	// Message of the Day [Valaris]
	for(i=0; motd_text[i][0] && i < MOTD_LINE_SIZE; i++) {
		if (battle_config.motd_type)
			clif_disp_onlyself(sd,motd_text[i],strlen(motd_text[i]));
		else
			clif_displaymessage(sd->fd, motd_text[i]);
	}

	// message of the limited time of the account
	if (expiration_time != 0) { // don't display if it's unlimited or unknow value
		char tmpstr[1024];
		strftime(tmpstr, sizeof(tmpstr) - 1, msg_txt(501), localtime(&expiration_time)); // "Your account time limit is: %d-%m-%Y %H:%M:%S."
		clif_wis_message(sd->fd, wisp_server_name, tmpstr, strlen(tmpstr)+1);
	}

	//Night message
	if (night_flag)
	{
		char tmpstr[1024];
		strcpy(tmpstr, msg_txt(500)); // Actually, it's the night...
		clif_wis_message(sd->fd, wisp_server_name, tmpstr, strlen(tmpstr)+1);
	}

	// Request all registries (auth is considered completed whence they arrive)
	intif_request_registry(sd,7);
	return true;
}

/*==========================================
 * Closes a connection because it failed to be authenticated from the char server.
 *------------------------------------------*/
void pc_authfail(struct map_session_data *sd)
{
	clif_authfail_fd(sd->fd, 0);
	return;
}

//Attempts to set a mob. 
int pc_set_hate_mob(struct map_session_data *sd, int pos, struct block_list *bl)
{
	int class_;
	if (!sd || !bl || pos < 0 || pos > 2)
		return 0;
	if (sd->hate_mob[pos] != -1)
	{	//Can't change hate targets.
		clif_hate_info(sd, pos, sd->hate_mob[pos], 0); //Display current
		return 0;
	}

	class_ = status_get_class(bl);
	if (!pcdb_checkid(class_)) {
		unsigned int max_hp = status_get_max_hp(bl);
		if ((pos == 1 && max_hp < 6000) || (pos == 2 && max_hp < 20000))
			return 0;
		if (pos != status_get_size(bl))
			return 0; //Wrong size
	}
	sd->hate_mob[pos] = class_;
	pc_setglobalreg(sd,sg_info[pos].hate_var,class_+1);
	clif_hate_info(sd, pos, class_, 1);
	return 1;
}

/*==========================================
 * Invoked once after the char/account/account2 registry variables are received. [Skotlex]
 *------------------------------------------*/
int pc_reg_received(struct map_session_data *sd)
{
	int i,j;
	
	sd->change_level = pc_readglobalreg(sd,"jobchange_level");
	sd->die_counter = pc_readglobalreg(sd,"PC_DIE_COUNTER");

	// Cash shop
	sd->cashPoints = pc_readaccountreg(sd,"#CASHPOINTS");
	sd->kafraPoints = pc_readaccountreg(sd,"#KAFRAPOINTS");

	// Cooking Exp
	sd->cook_mastery = pc_readglobalreg(sd,"COOK_MASTERY");

	if( (sd->class_&MAPID_BASEMASK) == MAPID_TAEKWON )
	{ // Better check for class rather than skill to prevent "skill resets" from unsetting this
		sd->mission_mobid = pc_readglobalreg(sd,"TK_MISSION_ID");
		sd->mission_count = pc_readglobalreg(sd,"TK_MISSION_COUNT");
	}

	//SG map and mob read [Komurka]
	for(i=0;i<3;i++) //for now - someone need to make reading from txt/sql
	{
		if ((j = pc_readglobalreg(sd,sg_info[i].feel_var))!=0) {
			sd->feel_map[i].index = j;
			sd->feel_map[i].m = map_mapindex2mapid(j);
		} else {
			sd->feel_map[i].index = 0;
			sd->feel_map[i].m = -1;
		}
		sd->hate_mob[i] = pc_readglobalreg(sd,sg_info[i].hate_var)-1;
	}

	if ((i = pc_checkskill(sd,RG_PLAGIARISM)) > 0) {
		sd->cloneskill_id = pc_readglobalreg(sd,"CLONE_SKILL");
		if (sd->cloneskill_id > 0) {
			sd->status.skill[sd->cloneskill_id].id = sd->cloneskill_id;
			sd->status.skill[sd->cloneskill_id].lv = pc_readglobalreg(sd,"CLONE_SKILL_LV");
			if (i < sd->status.skill[sd->cloneskill_id].lv)
				sd->status.skill[sd->cloneskill_id].lv = i;
			sd->status.skill[sd->cloneskill_id].flag = 13;	//cloneskill flag			
		}
	}

	//Weird... maybe registries were reloaded?
	if (sd->state.active)
		return 0;
	sd->state.active = 1;

	if (sd->status.party_id)
		party_member_joined(sd);
	if (sd->status.guild_id)
		guild_member_joined(sd);
	
	// pet
	if (sd->status.pet_id > 0)
		intif_request_petdata(sd->status.account_id, sd->status.char_id, sd->status.pet_id);

	// Homunculus [albator]
	if( sd->status.hom_id > 0 )
		intif_homunculus_requestload(sd->status.account_id, sd->status.hom_id);
	if( sd->status.mer_id > 0 )
		intif_mercenary_request(sd->status.mer_id, sd->status.char_id);

	map_addiddb(&sd->bl);
	map_delnickdb(sd->status.char_id, sd->status.name);
	if (!chrif_auth_finished(sd))
		ShowError("pc_reg_received: Failed to properly remove player %d:%d from logging db!\n", sd->status.account_id, sd->status.char_id);

	status_calc_pc(sd,1);
	chrif_scdata_request(sd->status.account_id, sd->status.char_id);

#ifndef TXT_ONLY
	intif_Mail_requestinbox(sd->status.char_id, 0); // MAIL SYSTEM - Request Mail Inbox
	intif_request_questlog(sd);
#endif

	if (sd->state.connect_new == 0 && sd->fd)
	{	//Character already loaded map! Gotta trigger LoadEndAck manually.
		sd->state.connect_new = 1;
		clif_parse_LoadEndAck(sd->fd, sd);
	}

#ifndef TXT_ONLY
	pc_inventory_rentals(sd);
#endif
	return 1;
}

static int pc_calc_skillpoint(struct map_session_data* sd)
{
	int  i,skill,inf2,skill_point=0;

	nullpo_retr(0, sd);

	for(i=1;i<MAX_SKILL;i++){
		if( (skill = pc_checkskill(sd,i)) > 0) {
			inf2 = skill_get_inf2(i);
			if((!(inf2&INF2_QUEST_SKILL) || battle_config.quest_skill_learn) &&
				!(inf2&(INF2_WEDDING_SKILL|INF2_SPIRIT_SKILL)) //Do not count wedding/link skills. [Skotlex]
				) {
				if(!sd->status.skill[i].flag)
					skill_point += skill;
				else if(sd->status.skill[i].flag > 2 && sd->status.skill[i].flag != 13) {
					skill_point += (sd->status.skill[i].flag - 2);
				}
			}
		}
	}

	return skill_point;
}


/*==========================================
 * ?¦çêéXLÌvZ
 *------------------------------------------*/
int pc_calc_skilltree(struct map_session_data *sd)
{
	int i,id=0,flag;
	int c=0;

	nullpo_retr(0, sd);
	i = pc_calc_skilltree_normalize_job(sd);
	c = pc_mapid2jobid(i, sd->status.sex);
	if( c == -1 )
	{ //Unable to normalize job??
		ShowError("pc_calc_skilltree: Unable to normalize job %d for character %s (%d:%d)\n", i, sd->status.name, sd->status.account_id, sd->status.char_id);
		return 1;
	}
	c = pc_class2idx(c);

	for( i = 0; i < MAX_SKILL; i++ )
	{ 
		if( sd->status.skill[i].flag != 13 ) //Don't touch plagiarized skills
			sd->status.skill[i].id = 0; //First clear skills.
	}

	for( i = 0; i < MAX_SKILL; i++ )
	{ 
		if( sd->status.skill[i].flag && sd->status.skill[i].flag != 13 )
		{ // Restore original level of skills after deleting earned skills.	
			sd->status.skill[i].lv = (sd->status.skill[i].flag == 1)?0:sd->status.skill[i].flag-2;
			sd->status.skill[i].flag = 0;
		}

		if( sd->sc.count && sd->sc.data[SC_SPIRIT] && sd->sc.data[SC_SPIRIT]->val2 == SL_BARDDANCER && i >= DC_HUMMING && i<= DC_SERVICEFORYOU )
		{ //Enable Bard/Dancer spirit linked skills.
			if( sd->status.sex )
			{ //Link dancer skills to bard.
				if( sd->status.skill[i-8].lv < 10 )
					continue;
				sd->status.skill[i].id = i;
				sd->status.skill[i].lv = sd->status.skill[i-8].lv; // Set the level to the same as the linking skill
				sd->status.skill[i].flag = 1; // Tag it as a non-savable, non-uppable, bonus skill
			}
			else
			{ //Link bard skills to dancer.
				if( sd->status.skill[i].lv < 10 )
					continue;
				sd->status.skill[i-8].id = i - 8;
				sd->status.skill[i-8].lv = sd->status.skill[i].lv; // Set the level to the same as the linking skill
				sd->status.skill[i-8].flag = 1; // Tag it as a non-savable, non-uppable, bonus skill
			}
		}
	}

	if( battle_config.gm_allskill > 0 && pc_isGM(sd) >= battle_config.gm_allskill )
	{
		for( i = 0; i < MAX_SKILL; i++ )
		{
			if( skill_get_inf2(i)&(INF2_NPC_SKILL|INF2_GUILD_SKILL) )
				continue; //Only skills you can't have are npc/guild ones
			if( skill_get_max(i) > 0 )
				sd->status.skill[i].id = i;
		}
		return 0;
	}

	do {
		flag = 0;
		for( i = 0; i < MAX_SKILL_TREE && (id = skill_tree[c][i].id) > 0; i++ )
		{
			int j, f, k, inf2;

			if( sd->status.skill[id].id )
				continue; //Skill already known.

			f = 1;
			if(!battle_config.skillfree) {
				for(j = 0; j < 5; j++) {
					if((k=skill_tree[c][i].need[j].id))
					{
						if (!sd->status.skill[k].id || sd->status.skill[k].flag == 13)
							k = 0; //Not learned.
						else if (sd->status.skill[k].flag) //Real lerned level
							k = sd->status.skill[skill_tree[c][i].need[j].id].flag-2;
						else
							k = pc_checkskill(sd,k);
						if (k < skill_tree[c][i].need[j].lv)
						{
							f = 0;
							break;
						}
					}
				}
				if( sd->status.job_level < skill_tree[c][i].joblv )
					f = 0; // job level requirement wasn't satisfied
			}

			if( f )
			{
				inf2 = skill_get_inf2(id);

				if(!sd->status.skill[id].lv && (
					(inf2&INF2_QUEST_SKILL && !battle_config.quest_skill_learn) ||
					inf2&INF2_WEDDING_SKILL ||
					(inf2&INF2_SPIRIT_SKILL && !sd->sc.data[SC_SPIRIT])
				))
					continue; //Cannot be learned via normal means. Note this check DOES allows raising already known skills.

				sd->status.skill[id].id = id;

				if(inf2&INF2_SPIRIT_SKILL)
				{	//Spirit skills cannot be learned, they will only show up on your tree when you get buffed.
					sd->status.skill[id].lv = 1; // need to manually specify a skill level
					sd->status.skill[id].flag = 1; //So it is not saved, and tagged as a "bonus" skill.
				}
				flag = 1; // skill list has changed, perform another pass
			}
		}
	} while(flag);

	// 
	if( c > 0 && (sd->class_&MAPID_UPPERMASK) == MAPID_TAEKWON && sd->status.base_level >= 90 && sd->status.skill_point == 0 && pc_famerank(sd->status.char_id, MAPID_TAEKWON) )
	{
		/* Taekwon Ranger Bonus Skill Tree
		============================================
		- Grant All Taekwon Tree, but only as Bonus Skills in case they drop from ranking.
		- (c > 0) to avoid grant Novice Skill Tree in case of Skill Reset (need more logic)
		- (sd->status.skill_point == 0) to wait until all skill points are asigned to avoid problems with Job Change quest. */

		for( i = 0; i < MAX_SKILL_TREE && (id = skill_tree[c][i].id) > 0; i++ )
		{
			if( (skill_get_inf2(id)&(INF2_QUEST_SKILL|INF2_WEDDING_SKILL)) )
				continue; //Do not include Quest/Wedding skills.

			if( sd->status.skill[id].id == 0 )
			{
				sd->status.skill[id].id = id;
				sd->status.skill[id].flag = 1; // So it is not saved, and tagged as a "bonus" skill.
			}
			else
				sd->status.skill[id].flag = sd->status.skill[id].lv+2;

			sd->status.skill[id].lv = skill_tree_get_max(id, sd->status.class_);
		}
	}

	return 0;
}

//Checks if you can learn a new skill after having leveled up a skill.
static void pc_check_skilltree(struct map_session_data *sd, int skill)
{
	int i,id=0,flag;
	int c=0;

	if(battle_config.skillfree)
		return; //Function serves no purpose if this is set
	
	i = pc_calc_skilltree_normalize_job(sd);
	c = pc_mapid2jobid(i, sd->status.sex);
	if (c == -1) { //Unable to normalize job??
		ShowError("pc_check_skilltree: Unable to normalize job %d for character %s (%d:%d)\n", i, sd->status.name, sd->status.account_id, sd->status.char_id);
		return;
	}
	c = pc_class2idx(c);
	do {
		flag = 0;
		for( i = 0; i < MAX_SKILL_TREE && (id=skill_tree[c][i].id)>0; i++ )
		{
			int j, f = 1, k;

			if( sd->status.skill[id].id ) //Already learned
				continue;
			
			for( j = 0; j < 5; j++ )
			{
				if( (k = skill_tree[c][i].need[j].id) )
				{
					if( !sd->status.skill[k].id || sd->status.skill[k].flag == 13 )
						k = 0; //Not learned.
					else if( sd->status.skill[k].flag ) //Real lerned level
						k = sd->status.skill[skill_tree[c][i].need[j].id].flag - 2;
					else
						k = pc_checkskill(sd,k);
					if( k < skill_tree[c][i].need[j].lv )
					{
						f = 0;
						break;
					}
				}
			}
			if( !f )
				continue;
			if( sd->status.job_level < skill_tree[c][i].joblv )
				continue;
			
			j = skill_get_inf2(id);
			if( !sd->status.skill[id].lv && (
				(j&INF2_QUEST_SKILL && !battle_config.quest_skill_learn) ||
				j&INF2_WEDDING_SKILL ||
				(j&INF2_SPIRIT_SKILL && !sd->sc.data[SC_SPIRIT])
			) )
				continue; //Cannot be learned via normal means.

			sd->status.skill[id].id = id;
			flag = 1;
		}
	} while(flag);
}

// Make sure all the skills are in the correct condition
// before persisting to the backend.. [MouseJstr]
int pc_clean_skilltree(struct map_session_data *sd)
{
	int i;
	for (i = 0; i < MAX_SKILL; i++){
		if (sd->status.skill[i].flag == 13 || sd->status.skill[i].flag == 1)
		{
			sd->status.skill[i].id = 0;
			sd->status.skill[i].lv = 0;
			sd->status.skill[i].flag = 0;
		} else if (sd->status.skill[i].flag){
			sd->status.skill[i].lv = sd->status.skill[i].flag-2;
			sd->status.skill[i].flag = 0;
		}
	}

	return 0;
}

int pc_calc_skilltree_normalize_job(struct map_session_data *sd)
{
	int skill_point;
	int c = sd->class_;
	
	if (!battle_config.skillup_limit)
		return c;
	
	skill_point = pc_calc_skillpoint(sd);
	if(pc_checkskill(sd, NV_BASIC) < 9) //Consider Novice Tree when you don't have NV_BASIC maxed.
		c = MAPID_NOVICE;
	else
	//Do not send S. Novices to first class (Novice)
	if ((sd->class_&JOBL_2) && (sd->class_&MAPID_UPPERMASK) != MAPID_SUPER_NOVICE &&
		sd->status.skill_point >= sd->status.job_level &&
		((sd->change_level > 0 && skill_point < sd->change_level+8) || skill_point < 58)) {
		//Send it to first class.
		c &= MAPID_BASEMASK;
	}
	if (sd->class_&JOBL_UPPER) //Convert to Upper
		c |= JOBL_UPPER;
	else if (sd->class_&JOBL_BABY) //Convert to Baby
		c |= JOBL_BABY;

	return c;
}

/*==========================================
 * Updates the weight status
 *------------------------------------------
 * 1: overweight 50%
 * 2: overweight 90%
 * It's assumed that SC_WEIGHT50 and SC_WEIGHT90 are only started/stopped here.
 */
int pc_updateweightstatus(struct map_session_data *sd)
{
	int old_overweight;
	int new_overweight;

	nullpo_retr(1, sd);

	old_overweight = (sd->sc.data[SC_WEIGHT90]) ? 2 : (sd->sc.data[SC_WEIGHT50]) ? 1 : 0;
	new_overweight = (pc_is90overweight(sd)) ? 2 : (pc_is50overweight(sd)) ? 1 : 0;

	if( old_overweight == new_overweight )
		return 0; // no change

	// stop old status change
	if( old_overweight == 1 )
		status_change_end(&sd->bl, SC_WEIGHT50, -1);
	else if( old_overweight == 2 )
		status_change_end(&sd->bl, SC_WEIGHT90, -1);

	// start new status change
	if( new_overweight == 1 )
		sc_start(&sd->bl, SC_WEIGHT50, 100, 0, 0);
	else if( new_overweight == 2 )
		sc_start(&sd->bl, SC_WEIGHT90, 100, 0, 0);

	// update overweight status
	sd->regen.state.overweight = new_overweight;

	return 0;
}

int pc_disguise(struct map_session_data *sd, int class_)
{
	if (!class_ && !sd->disguise)
		return 0;
	if (class_ && sd->disguise == class_)
		return 0;

	if(sd->sc.option&OPTION_INVISIBLE)
  	{	//Character is invisible. Stealth class-change. [Skotlex]
		sd->disguise = class_; //viewdata is set on uncloaking.
		return 2;
	}

	if (sd->bl.prev != NULL) {
		pc_stop_walking(sd, 0);
		clif_clearunit_area(&sd->bl, 0);
	}

	if (!class_) {
		sd->disguise = 0;
		class_ = sd->status.class_;
	} else
		sd->disguise=class_;

	status_set_viewdata(&sd->bl, class_);
	clif_changeoption(&sd->bl);

	if (sd->bl.prev != NULL) {
		clif_spawn(&sd->bl);
		if (class_ == sd->status.class_ && pc_iscarton(sd))
		{	//It seems the cart info is lost on undisguise.
			clif_cartlist(sd);
			clif_updatestatus(sd,SP_CARTINFO);
		}
	}
	return 1;
}

static int pc_bonus_autospell(struct s_autospell *spell, int max, short id, short lv, short rate, short flag, short card_id)
{
	int i;

	if( !rate )
		return 0;

	for( i = 0; i < max && spell[i].id; i++ )
	{
		if( (spell[i].card_id == card_id || spell[i].rate < 0 || rate < 0) && spell[i].id == id && spell[i].lv == lv )
		{
			if( !battle_config.autospell_stacking && spell[i].rate > 0 && rate > 0 )
				return 0;
			rate += spell[i].rate;
			break;
		}
	}
	if (i == max) {
		ShowWarning("pc_bonus: Reached max (%d) number of autospells per character!\n", max);
		return 0;
	}
	spell[i].id = id;
	spell[i].lv = lv;
	spell[i].rate = rate;
	//Auto-update flag value.
	if (!(flag&BF_RANGEMASK)) flag|=BF_SHORT|BF_LONG; //No range defined? Use both.
	if (!(flag&BF_WEAPONMASK)) flag|=BF_WEAPON; //No attack type defined? Use weapon.
	if (!(flag&BF_SKILLMASK)) {
		if (flag&(BF_MAGIC|BF_MISC)) flag|=BF_SKILL; //These two would never trigger without BF_SKILL
		if (flag&BF_WEAPON) flag|=BF_NORMAL; //By default autospells should only trigger on normal weapon attacks.
	}
	spell[i].flag|= flag;
	spell[i].card_id = card_id;
	return 1;
}

static int pc_bonus_autospell_onskill(struct s_autospell *spell, int max, short src_skill, short id, short lv, short rate, short card_id)
{
	int i;

	if( !rate )
		return 0;

	for( i = 0; i < max && spell[i].id; i++ )  
	{  
		if( spell[i].flag == src_skill && spell[i].id == id && spell[i].lv == lv && (spell[i].card_id == card_id || spell[i].rate <= 0 || rate < 0) )  
		{  
			if( !battle_config.autospell_stacking && spell[i].rate > 0 && rate > 0 )
				return 0;
			rate += spell[i].rate;
			break; 
		}  
	}

	if( i == max )
	{
		ShowWarning("pc_bonus: Reached max (%d) number of autospells per character!\n", max);
		return 0;
	}

	spell[i].flag = src_skill;
	spell[i].id	= id;
	spell[i].lv = lv;
	spell[i].rate = rate;
	spell[i].card_id = card_id;
	return 1;
}

static int pc_bonus_addeff(struct s_addeffect* effect, int max, enum sc_type id, short rate, short arrow_rate, unsigned char flag)
{
	int i;
	if (!(flag&(ATF_SHORT|ATF_LONG)))
		flag|=ATF_SHORT|ATF_LONG; //Default range: both
	if (!(flag&(ATF_TARGET|ATF_SELF)))
		flag|=ATF_TARGET; //Default target: enemy.
	if (!(flag&(ATF_WEAPON|ATF_MAGIC|ATF_MISC)))
		flag|=ATF_WEAPON; //Defatul type: weapon.

	for (i = 0; i < max && effect[i].flag; i++) {
		if (effect[i].id == id && effect[i].flag == flag)
		{
			effect[i].rate += rate;
			effect[i].arrow_rate += arrow_rate;
			return 1;
		}
	}
	if (i == max) {
		ShowWarning("pc_bonus: Reached max (%d) number of add effects per character!\n", max);
		return 0;
	}
	effect[i].id = id;
	effect[i].rate = rate;
	effect[i].arrow_rate = arrow_rate;
	effect[i].flag = flag;
	return 1;
}

static int pc_bonus_addeff_onskill(struct s_addeffectonskill* effect, int max, enum sc_type id, short rate, short skill, unsigned char target)
{
	int i;
	for( i = 0; i < max && effect[i].skill; i++ )
	{
		if( effect[i].id == id && effect[i].skill == skill && effect[i].target == target )
		{
			effect[i].rate += rate;
			return 1;
		}
	}
	if( i == max ) {
		ShowWarning("pc_bonus: Reached max (%d) number of add effects on skill per character!\n", max);
		return 0;
	}
	effect[i].id = id;
	effect[i].rate = rate;
	effect[i].skill = skill;
	effect[i].target = target;
	return 1;
}

static int pc_bonus_item_drop(struct s_add_drop *drop, const short max, short id, short group, int race, int rate)
{
	int i;
	//Apply config rate adjustment settings.
	if (rate >= 0) { //Absolute drop.
		if (battle_config.item_rate_adddrop != 100)
			rate = rate*battle_config.item_rate_adddrop/100;
		if (rate < battle_config.item_drop_adddrop_min)
			rate = battle_config.item_drop_adddrop_min;
		else if (rate > battle_config.item_drop_adddrop_max)
			rate = battle_config.item_drop_adddrop_max;
	} else { //Relative drop, max/min limits are applied at drop time.
		if (battle_config.item_rate_adddrop != 100)
			rate = rate*battle_config.item_rate_adddrop/100;
		if (rate > -1)
			rate = -1;
	}
	for(i = 0; i < max && (drop[i].id || drop[i].group); i++) {
		if(
			(id && drop[i].id == id) ||
			(group && drop[i].group == group)
		) {
			drop[i].race |= race;
			if(drop[i].rate > 0 && rate > 0)
			{	//Both are absolute rates.
				if (drop[i].rate < rate)
					drop[i].rate = rate;
			} else
			if(drop[i].rate < 0 && rate < 0) {
				//Both are relative rates.
				if (drop[i].rate > rate)
					drop[i].rate = rate;
			} else if (rate < 0) //Give preference to relative rate.
					drop[i].rate = rate;
			return 1;
		}
	}
	if(i == max) {
		ShowWarning("pc_bonus: Reached max (%d) number of added drops per character!\n", max);
		return 0;
	}
	drop[i].id = id;
	drop[i].group = group;
	drop[i].race |= race;
	drop[i].rate = rate;
	return 1;
}

int pc_addautobonus(struct s_autobonus *bonus,char max,const char *script,short rate,unsigned int dur,short flag,const char *other_script,unsigned short pos,bool onskill)
{
	int i;

	ARR_FIND(0, max, i, bonus[i].rate == 0);
	if( i == max )
	{
		ShowWarning("pc_addautobonus: Reached max (%d) number of autobonus per character!\n", max);
		return 0;
	}

	if( !onskill )
	{
		if( !(flag&BF_RANGEMASK) )
			flag|=BF_SHORT|BF_LONG; //No range defined? Use both.
		if( !(flag&BF_WEAPONMASK) )
			flag|=BF_WEAPON; //No attack type defined? Use weapon.
		if( !(flag&BF_SKILLMASK) )
		{
			if( flag&(BF_MAGIC|BF_MISC) )
				flag|=BF_SKILL; //These two would never trigger without BF_SKILL
			if( flag&BF_WEAPON )
				flag|=BF_NORMAL|BF_SKILL;
		}
	}

	bonus[i].rate = rate;
	bonus[i].duration = dur;
	bonus[i].active = INVALID_TIMER;
	bonus[i].atk_type = flag;
	bonus[i].pos = pos;
	bonus[i].bonus_script = aStrdup(script);
	bonus[i].other_script = other_script?aStrdup(other_script):NULL;
	return 1;
}

int pc_delautobonus(struct map_session_data* sd, struct s_autobonus *autobonus,char max,bool restore)
{
	int i;
	nullpo_retr(0, sd);

	for( i = 0; i < max; i++ )
	{
		if( autobonus[i].active != INVALID_TIMER )
		{
			if( restore && sd->state.autobonus&autobonus[i].pos )
			{
				if( autobonus[i].bonus_script )
				{
					int j;
					ARR_FIND( 0, EQI_MAX-1, j, sd->equip_index[j] >= 0 && sd->status.inventory[sd->equip_index[j]].equip == autobonus[i].pos );
					if( j < EQI_MAX-1 )
						script_run_autobonus(autobonus[i].bonus_script,sd->bl.id,sd->equip_index[j]);
				}
				continue;
			}
			else
			{ // Logout / Unequipped an item with an activated bonus
				delete_timer(autobonus[i].active,pc_endautobonus);
				autobonus[i].active = INVALID_TIMER;
			}
		}

		if( autobonus[i].bonus_script ) aFree(autobonus[i].bonus_script);
		if( autobonus[i].other_script ) aFree(autobonus[i].other_script);
		autobonus[i].bonus_script = autobonus[i].other_script = NULL;
		autobonus[i].rate = autobonus[i].atk_type = autobonus[i].duration = autobonus[i].pos = 0;
		autobonus[i].active = INVALID_TIMER;
	}

	return 0;
}

int pc_exeautobonus(struct map_session_data *sd,struct s_autobonus *autobonus)
{
	nullpo_retr(0, sd);
	nullpo_retr(0, autobonus);

	if( autobonus->other_script )
	{
		int j;
		ARR_FIND( 0, EQI_MAX-1, j, sd->equip_index[j] >= 0 && sd->status.inventory[sd->equip_index[j]].equip == autobonus->pos );
		if( j < EQI_MAX-1 )
			script_run_autobonus(autobonus->other_script,sd->bl.id,sd->equip_index[j]);
	}

	autobonus->active = add_timer(gettick()+autobonus->duration, pc_endautobonus, sd->bl.id, (intptr)autobonus);
	sd->state.autobonus |= autobonus->pos;
	status_calc_pc(sd,0);

	return 0;
}

int pc_endautobonus(int tid, unsigned int tick, int id, intptr data)
{
	struct map_session_data *sd = map_id2sd(id);
	struct s_autobonus *autobonus = (struct s_autobonus *)data;

	nullpo_retr(0, sd);
	nullpo_retr(0, autobonus);

	autobonus->active = INVALID_TIMER;
	sd->state.autobonus &= ~autobonus->pos;
	status_calc_pc(sd,0);
	return 0;
}

/*==========================================
 * ? õiÉæé\ÍÌ{?iXÝè
 *------------------------------------------*/
int pc_bonus(struct map_session_data *sd,int type,int val)
{
	struct status_data *status;
	int bonus;
	nullpo_retr(0, sd);

	status = &sd->base_status;

	switch(type){
	case SP_STR:
	case SP_AGI:
	case SP_VIT:
	case SP_INT:
	case SP_DEX:
	case SP_LUK:
		if(sd->state.lr_flag != 2)
			sd->param_bonus[type-SP_STR]+=val;
		break;
	case SP_ATK1:
		if(!sd->state.lr_flag) {
			bonus = status->rhw.atk + val;
			status->rhw.atk = cap_value(bonus, 0, USHRT_MAX);
		}
		else if(sd->state.lr_flag == 1) {
			bonus = status->lhw.atk + val;
			status->lhw.atk =  cap_value(bonus, 0, USHRT_MAX);
		}
		break;
	case SP_ATK2:
		if(!sd->state.lr_flag) {
			bonus = status->rhw.atk2 + val;
			status->rhw.atk2 = cap_value(bonus, 0, USHRT_MAX);
		}
		else if(sd->state.lr_flag == 1) {
			bonus = status->lhw.atk2 + val;
			status->lhw.atk2 =  cap_value(bonus, 0, USHRT_MAX);
		}
		break;
	case SP_BASE_ATK:
		if(sd->state.lr_flag != 2) {
			bonus = status->batk + val;
			status->batk = cap_value(bonus, 0, USHRT_MAX);
		}
		break;
	case SP_DEF1:
		if(sd->state.lr_flag != 2) {
			bonus = status->def + val;
			status->def = cap_value(bonus, CHAR_MIN, CHAR_MAX);
		}
		break;
	case SP_DEF2:
		if(sd->state.lr_flag != 2) {
			bonus = status->def2 + val;
			status->def2 = cap_value(bonus, SHRT_MIN, SHRT_MAX);
		}
		break;
	case SP_MDEF1:
		if(sd->state.lr_flag != 2) {
			bonus = status->mdef + val;
			status->mdef = cap_value(bonus, CHAR_MIN, CHAR_MAX);
		}
		break;
	case SP_MDEF2:
		if(sd->state.lr_flag != 2) {
			bonus = status->mdef2 + val;
			status->mdef2 = cap_value(bonus, SHRT_MIN, SHRT_MAX);
		}
		break;
	case SP_HIT:
		if(sd->state.lr_flag != 2) {
			bonus = status->hit + val;
			status->hit = cap_value(bonus, SHRT_MIN, SHRT_MAX);
		} else
			sd->arrow_hit+=val;
		break;
	case SP_FLEE1:
		if(sd->state.lr_flag != 2) {
			bonus = status->flee + val;
			status->flee = cap_value(bonus, SHRT_MIN, SHRT_MAX);
		}
		break;
	case SP_FLEE2:
		if(sd->state.lr_flag != 2) {
			bonus = status->flee2 + val*10;
			status->flee2 = cap_value(bonus, SHRT_MIN, SHRT_MAX);
		}
		break;
	case SP_CRITICAL:
		if(sd->state.lr_flag != 2) {
			bonus = status->cri + val*10;
			status->cri = cap_value(bonus, SHRT_MIN, SHRT_MAX);
		} else
			sd->arrow_cri += val*10;
		break;
	case SP_ATKELE:
		if(val >= ELE_MAX) {
			ShowError("pc_bonus: SP_ATKELE: Invalid element %d\n", val);
			break;
		}
		switch (sd->state.lr_flag)
		{
		case 2:
			switch (sd->status.weapon) {
				case W_BOW:
				case W_REVOLVER:
				case W_RIFLE:
				case W_GATLING:
				case W_SHOTGUN:
				case W_GRENADE:
					//Become weapon element.
					status->rhw.ele=val;
					break;
				default: //Become arrow element.
					sd->arrow_ele=val;
					break;
			}
			break;
		case 1:
			status->lhw.ele=val;
			break;
		default:
			status->rhw.ele=val;
			break;
		}
		break;
	case SP_DEFELE:
		if(val >= ELE_MAX) {
			ShowError("pc_bonus: SP_DEFELE: Invalid element %d\n", val);
			break;
		}
		if(sd->state.lr_flag != 2)
			status->def_ele=val;
		break;
	case SP_MAXHP:
		if(sd->state.lr_flag == 2)
			break;
		val += (int)status->max_hp;
		//Negative bonuses will underflow, this will be handled in status_calc_pc through casting 
		//If this is called outside of status_calc_pc, you'd better pray they do not underflow and end with UINT_MAX max_hp.
		status->max_hp = (unsigned int)val;
		break;
	case SP_MAXSP:
		if(sd->state.lr_flag == 2) 
			break;
		val += (int)status->max_sp;
		status->max_sp = (unsigned int)val;
		break;
	case SP_CASTRATE:
		if(sd->state.lr_flag != 2)
			sd->castrate+=val;
		break;
	case SP_MAXHPRATE:
		if(sd->state.lr_flag != 2)
			sd->hprate+=val;
		break;
	case SP_MAXSPRATE:
		if(sd->state.lr_flag != 2)
			sd->sprate+=val;
		break;
	case SP_SPRATE:
		if(sd->state.lr_flag != 2)
			sd->dsprate+=val;
		break;
	case SP_ATTACKRANGE:
		switch (sd->state.lr_flag) {
		case 2:
			switch (sd->status.weapon) {
				case W_BOW:
				case W_REVOLVER:
				case W_RIFLE:
				case W_GATLING:
				case W_SHOTGUN:
				case W_GRENADE:
					status->rhw.range += val;
			}
			break;
		case 1:
			status->lhw.range += val;
			break;
		default:
			status->rhw.range += val;
			break;
		}
		break;
	case SP_SPEED_RATE:	//Non stackable increase
		if(sd->state.lr_flag != 2)
			sd->speed_rate = min(sd->speed_rate, -val);
		break;
	case SP_SPEED_ADDRATE:	//Stackable increase
		if(sd->state.lr_flag != 2)
			sd->speed_add_rate -= val;
		break;
	case SP_ASPD:	//Raw increase
		if(sd->state.lr_flag != 2)
			sd->aspd_add -= 10*val;
		break;
	case SP_ASPD_RATE:	//Stackable increase - Made it linear as per rodatazone
		if(sd->state.lr_flag != 2)
			status->aspd_rate -= 10*val;
		break;
	case SP_HP_RECOV_RATE:
		if(sd->state.lr_flag != 2)
			sd->hprecov_rate += val;
		break;
	case SP_SP_RECOV_RATE:
		if(sd->state.lr_flag != 2)
			sd->sprecov_rate += val;
		break;
	case SP_CRITICAL_DEF:
		if(sd->state.lr_flag != 2)
			sd->critical_def += val;
		break;
	case SP_NEAR_ATK_DEF:
		if(sd->state.lr_flag != 2)
			sd->near_attack_def_rate += val;
		break;
	case SP_LONG_ATK_DEF:
		if(sd->state.lr_flag != 2)
			sd->long_attack_def_rate += val;
		break;
	case SP_DOUBLE_RATE:
		if(sd->state.lr_flag == 0 && sd->double_rate < val)
			sd->double_rate = val;
		break;
	case SP_DOUBLE_ADD_RATE:
		if(sd->state.lr_flag == 0)
			sd->double_add_rate += val;
		break;
	case SP_MATK_RATE:
		if(sd->state.lr_flag != 2)
			sd->matk_rate += val;
		break;
	case SP_IGNORE_DEF_ELE:
		if(val >= ELE_MAX) {
			ShowError("pc_bonus: SP_IGNORE_DEF_ELE: Invalid element %d\n", val);
			break;
		}
		if(!sd->state.lr_flag)
			sd->right_weapon.ignore_def_ele |= 1<<val;
		else if(sd->state.lr_flag == 1)
			sd->left_weapon.ignore_def_ele |= 1<<val;
		break;
	case SP_IGNORE_DEF_RACE:
		if(!sd->state.lr_flag)
			sd->right_weapon.ignore_def_race |= 1<<val;
		else if(sd->state.lr_flag == 1)
			sd->left_weapon.ignore_def_race |= 1<<val;
		break;
	case SP_ATK_RATE:
		if(sd->state.lr_flag != 2)
			sd->atk_rate += val;
		break;
	case SP_MAGIC_ATK_DEF:
		if(sd->state.lr_flag != 2)
			sd->magic_def_rate += val;
		break;
	case SP_MISC_ATK_DEF:
		if(sd->state.lr_flag != 2)
			sd->misc_def_rate += val;
		break;
	case SP_IGNORE_MDEF_RATE:
		if(sd->state.lr_flag != 2) {
			sd->ignore_mdef[RC_NONBOSS] += val;
			sd->ignore_mdef[RC_BOSS] += val;
		}
		break;
	case SP_IGNORE_MDEF_ELE:
		if(val >= ELE_MAX) {
			ShowError("pc_bonus: SP_IGNORE_MDEF_ELE: Invalid element %d\n", val);
			break;
		}
		if(sd->state.lr_flag != 2)
			sd->ignore_mdef_ele |= 1<<val;
		break;
	case SP_IGNORE_MDEF_RACE:
		if(sd->state.lr_flag != 2)
			sd->ignore_mdef_race |= 1<<val;
		break;
	case SP_PERFECT_HIT_RATE:
		if(sd->state.lr_flag != 2 && sd->perfect_hit < val)
			sd->perfect_hit = val;
		break;
	case SP_PERFECT_HIT_ADD_RATE:
		if(sd->state.lr_flag != 2)
			sd->perfect_hit_add += val;
		break;
	case SP_CRITICAL_RATE:
		if(sd->state.lr_flag != 2)
			sd->critical_rate+=val;
		break;
	case SP_DEF_RATIO_ATK_ELE:
		if(val >= ELE_MAX) {
			ShowError("pc_bonus: SP_DEF_RATIO_ATK_ELE: Invalid element %d\n", val);
			break;
		}
		if(!sd->state.lr_flag)
			sd->right_weapon.def_ratio_atk_ele |= 1<<val;
		else if(sd->state.lr_flag == 1)
			sd->left_weapon.def_ratio_atk_ele |= 1<<val;
		break;
	case SP_DEF_RATIO_ATK_RACE:
		if(val >= RC_MAX) {
			ShowError("pc_bonus: SP_DEF_RATIO_ATK_RACE: Invalid race %d\n", val);
			break;
		}
		if(!sd->state.lr_flag)
			sd->right_weapon.def_ratio_atk_race |= 1<<val;
		else if(sd->state.lr_flag == 1)
			sd->left_weapon.def_ratio_atk_race |= 1<<val;
		break;
	case SP_HIT_RATE:
		if(sd->state.lr_flag != 2)
			sd->hit_rate += val;
		break;
	case SP_FLEE_RATE:
		if(sd->state.lr_flag != 2)
			sd->flee_rate += val;
		break;
	case SP_FLEE2_RATE:
		if(sd->state.lr_flag != 2)
			sd->flee2_rate += val;
		break;
	case SP_DEF_RATE:
		if(sd->state.lr_flag != 2)
			sd->def_rate += val;
		break;
	case SP_DEF2_RATE:
		if(sd->state.lr_flag != 2)
			sd->def2_rate += val;
		break;
	case SP_MDEF_RATE:
		if(sd->state.lr_flag != 2)
			sd->mdef_rate += val;
		break;
	case SP_MDEF2_RATE:
		if(sd->state.lr_flag != 2)
			sd->mdef2_rate += val;
		break;
	case SP_RESTART_FULL_RECOVER:
		if(sd->state.lr_flag != 2)
			sd->special_state.restart_full_recover = 1;
		break;
	case SP_NO_CASTCANCEL:
		if(sd->state.lr_flag != 2)
			sd->special_state.no_castcancel = 1;
		break;
	case SP_NO_CASTCANCEL2:
		if(sd->state.lr_flag != 2)
			sd->special_state.no_castcancel2 = 1;
		break;
	case SP_NO_SIZEFIX:
		if(sd->state.lr_flag != 2)
			sd->special_state.no_sizefix = 1;
		break;
	case SP_NO_MAGIC_DAMAGE:
		if(sd->state.lr_flag == 2)
			break;
		val+= sd->special_state.no_magic_damage;
		sd->special_state.no_magic_damage = cap_value(val,0,100);
		break;
	case SP_NO_WEAPON_DAMAGE:
		if(sd->state.lr_flag == 2)
			break;
		val+= sd->special_state.no_weapon_damage;
		sd->special_state.no_weapon_damage = cap_value(val,0,100);
		break;
	case SP_NO_MISC_DAMAGE:
		if(sd->state.lr_flag == 2)
			break;
		val+= sd->special_state.no_misc_damage;
		sd->special_state.no_misc_damage = cap_value(val,0,100);
		break;
	case SP_NO_GEMSTONE:
		if(sd->state.lr_flag != 2)
			sd->special_state.no_gemstone = 1;
		break;
	case SP_INTRAVISION: // Maya Purple Card effect allowing to see Hiding/Cloaking people [DracoRPG]
		if(sd->state.lr_flag != 2) {
			sd->special_state.intravision = 1;
			clif_status_load(&sd->bl, SI_INTRAVISION, 1);
		}
		break;
	case SP_NO_KNOCKBACK:
		if(sd->state.lr_flag != 2)
			sd->special_state.no_knockback = 1;
		break;
	case SP_SPLASH_RANGE:
		if(sd->state.lr_flag != 2 && sd->splash_range < val)
			sd->splash_range = val;
		break;
	case SP_SPLASH_ADD_RANGE:
		if(sd->state.lr_flag != 2)
			sd->splash_add_range += val;
		break;
	case SP_SHORT_WEAPON_DAMAGE_RETURN:
		if(sd->state.lr_flag != 2)
			sd->short_weapon_damage_return += val;
		break;
	case SP_LONG_WEAPON_DAMAGE_RETURN:
		if(sd->state.lr_flag != 2)
			sd->long_weapon_damage_return += val;
		break;
	case SP_MAGIC_DAMAGE_RETURN: //AppleGirl Was Here
		if(sd->state.lr_flag != 2)
			sd->magic_damage_return += val;
		break;
	case SP_ALL_STATS:	// [Valaris]
		if(sd->state.lr_flag!=2) {
			sd->param_bonus[SP_STR-SP_STR]+=val;
			sd->param_bonus[SP_AGI-SP_STR]+=val;
			sd->param_bonus[SP_VIT-SP_STR]+=val;
			sd->param_bonus[SP_INT-SP_STR]+=val;
			sd->param_bonus[SP_DEX-SP_STR]+=val;
			sd->param_bonus[SP_LUK-SP_STR]+=val;
		}
		break;
	case SP_AGI_VIT:	// [Valaris]
		if(sd->state.lr_flag!=2) {
			sd->param_bonus[SP_AGI-SP_STR]+=val;
			sd->param_bonus[SP_VIT-SP_STR]+=val;
		}
		break;
	case SP_AGI_DEX_STR:	// [Valaris]
		if(sd->state.lr_flag!=2) {
			sd->param_bonus[SP_AGI-SP_STR]+=val;
			sd->param_bonus[SP_DEX-SP_STR]+=val;
			sd->param_bonus[SP_STR-SP_STR]+=val;
		}
		break;
	case SP_PERFECT_HIDE: // [Valaris]
		if(sd->state.lr_flag!=2)
			sd->special_state.perfect_hiding=1;
		break;
	case SP_UNBREAKABLE:
		if(sd->state.lr_flag!=2)
			sd->unbreakable += val;
		break;
	case SP_UNBREAKABLE_WEAPON:
		if(sd->state.lr_flag != 2)
			sd->unbreakable_equip |= EQP_WEAPON;
		break;
	case SP_UNBREAKABLE_ARMOR:
		if(sd->state.lr_flag != 2)
			sd->unbreakable_equip |= EQP_ARMOR;
		break;
	case SP_UNBREAKABLE_HELM:
		if(sd->state.lr_flag != 2)
			sd->unbreakable_equip |= EQP_HELM;
		break;
	case SP_UNBREAKABLE_SHIELD:
		if(sd->state.lr_flag != 2)
			sd->unbreakable_equip |= EQP_SHIELD;
		break;
	case SP_UNBREAKABLE_GARMENT:
		if(sd->state.lr_flag != 2)
			sd->unbreakable_equip |= EQP_GARMENT;
		break;
	case SP_UNBREAKABLE_SHOES:
		if(sd->state.lr_flag != 2)
			sd->unbreakable_equip |= EQP_SHOES;
		break;
	case SP_CLASSCHANGE: // [Valaris]
		if(sd->state.lr_flag !=2)
			sd->classchange=val;
		break;
	case SP_LONG_ATK_RATE:
		if(sd->state.lr_flag != 2)	//[Lupus] it should stack, too. As any other cards rate bonuses
			sd->long_attack_atk_rate+=val;
		break;
	case SP_BREAK_WEAPON_RATE:
		if(sd->state.lr_flag != 2)
			sd->break_weapon_rate+=val;
		break;
	case SP_BREAK_ARMOR_RATE:
		if(sd->state.lr_flag != 2)
			sd->break_armor_rate+=val;
		break;
	case SP_ADD_STEAL_RATE:
		if(sd->state.lr_flag != 2)
			sd->add_steal_rate+=val;
		break;
	case SP_DELAYRATE:
		if(sd->state.lr_flag != 2)
			sd->delayrate+=val;
		break;
	case SP_CRIT_ATK_RATE:
		if(sd->state.lr_flag != 2)
			sd->crit_atk_rate += val;
		break;
	case SP_NO_REGEN:
		if(sd->state.lr_flag != 2)
			sd->regen.state.block|=val;
		break;
	case SP_UNSTRIPABLE_WEAPON:
		if(sd->state.lr_flag != 2)
			sd->unstripable_equip |= EQP_WEAPON;
		break;
	case SP_UNSTRIPABLE:
	case SP_UNSTRIPABLE_ARMOR:
		if(sd->state.lr_flag != 2)
			sd->unstripable_equip |= EQP_ARMOR;
		break;
	case SP_UNSTRIPABLE_HELM:
		if(sd->state.lr_flag != 2)
			sd->unstripable_equip |= EQP_HELM;
		break;
	case SP_UNSTRIPABLE_SHIELD:
		if(sd->state.lr_flag != 2)
			sd->unstripable_equip |= EQP_SHIELD;
		break;
	case SP_HP_DRAIN_VALUE:
		if(!sd->state.lr_flag) {
			sd->right_weapon.hp_drain[RC_NONBOSS].value += val;
			sd->right_weapon.hp_drain[RC_BOSS].value += val;
		}
		else if(sd->state.lr_flag == 1) {
			sd->right_weapon.hp_drain[RC_NONBOSS].value += val;
			sd->right_weapon.hp_drain[RC_BOSS].value += val;
		}
		break;
	case SP_SP_DRAIN_VALUE:
		if(!sd->state.lr_flag) {
			sd->right_weapon.sp_drain[RC_NONBOSS].value += val;
			sd->right_weapon.sp_drain[RC_BOSS].value += val;
		}
		else if(sd->state.lr_flag == 1) {
			sd->left_weapon.sp_drain[RC_NONBOSS].value += val;
			sd->left_weapon.sp_drain[RC_BOSS].value += val;
		}
		break;
	case SP_SP_GAIN_VALUE:
		if(!sd->state.lr_flag)
			sd->sp_gain_value += val;
		break;
	case SP_HP_GAIN_VALUE:
		if(!sd->state.lr_flag)
			sd->hp_gain_value += val;
		break;
	case SP_MAGIC_SP_GAIN_VALUE:
		if(!sd->state.lr_flag)
			sd->magic_sp_gain_value += val;
		break;
	case SP_MAGIC_HP_GAIN_VALUE:
		if(!sd->state.lr_flag)
			sd->magic_hp_gain_value += val;
		break;
	case SP_ADD_HEAL_RATE:
		if(sd->state.lr_flag != 2)
			sd->add_heal_rate += val;
		break;
	case SP_ADD_HEAL2_RATE:
		if(sd->state.lr_flag != 2)
			sd->add_heal2_rate += val;
		break;
	default:
		ShowWarning("pc_bonus: unknown type %d %d !\n",type,val);
		break;
	}
	return 0;
}

/*==========================================
 * ? õiÉæé\ÍÌ{?iXÝè
 *------------------------------------------*/
int pc_bonus2(struct map_session_data *sd,int type,int type2,int val)
{
	int i;

	nullpo_retr(0, sd);

	switch(type){
	case SP_ADDELE:
		if(type2 >= ELE_MAX) {
			ShowError("pc_bonus2: SP_ADDELE: Invalid element %d\n", type2);
			break;
		}
		if(!sd->state.lr_flag)
			sd->right_weapon.addele[type2]+=val;
		else if(sd->state.lr_flag == 1)
			sd->left_weapon.addele[type2]+=val;
		else if(sd->state.lr_flag == 2)
			sd->arrow_addele[type2]+=val;
		break;
	case SP_ADDRACE:
		if(!sd->state.lr_flag)
			sd->right_weapon.addrace[type2]+=val;
		else if(sd->state.lr_flag == 1)
			sd->left_weapon.addrace[type2]+=val;
		else if(sd->state.lr_flag == 2)
			sd->arrow_addrace[type2]+=val;
		break;
	case SP_ADDSIZE:
		if(!sd->state.lr_flag)
			sd->right_weapon.addsize[type2]+=val;
		else if(sd->state.lr_flag == 1)
			sd->left_weapon.addsize[type2]+=val;
		else if(sd->state.lr_flag == 2)
			sd->arrow_addsize[type2]+=val;
		break;
	case SP_SUBELE:
		if(type2 >= ELE_MAX) {
			ShowError("pc_bonus2: SP_SUBELE: Invalid element %d\n", type2);
			break;
		}
		if(sd->state.lr_flag != 2)
			sd->subele[type2]+=val;
		break;
	case SP_SUBRACE:
		if(sd->state.lr_flag != 2)
			sd->subrace[type2]+=val;
		break;
	case SP_ADDEFF:
		if (type2 > SC_MAX) {
			ShowWarning("pc_bonus2 (Add Effect): %d is not supported.\n", type2);
			break;
		}
		pc_bonus_addeff(sd->addeff, ARRAYLENGTH(sd->addeff), (sc_type)type2,
			sd->state.lr_flag!=2?val:0, sd->state.lr_flag==2?val:0, 0);
		break;
	case SP_ADDEFF2:
		if (type2 > SC_MAX) {
			ShowWarning("pc_bonus2 (Add Effect2): %d is not supported.\n", type2);
			break;
		}
		pc_bonus_addeff(sd->addeff, ARRAYLENGTH(sd->addeff), (sc_type)type2,
			sd->state.lr_flag!=2?val:0, sd->state.lr_flag==2?val:0, ATF_SELF);
		break;
	case SP_RESEFF:
		if (type2 < SC_COMMON_MIN || type2 > SC_COMMON_MAX) {
			ShowWarning("pc_bonus2 (Resist Effect): %d is not supported.\n", type2);
			break;
		}
		if(sd->state.lr_flag == 2)
			break;
		i = sd->reseff[type2-SC_COMMON_MIN]+val;
		sd->reseff[type2-SC_COMMON_MIN]= cap_value(i, 0, 10000);
		break;
	case SP_MAGIC_ADDELE:
		if(type2 >= ELE_MAX) {
			ShowError("pc_bonus2: SP_MAGIC_ADDELE: Invalid element %d\n", type2);
			break;
		}
		if(sd->state.lr_flag != 2)
			sd->magic_addele[type2]+=val;
		break;
	case SP_MAGIC_ADDRACE:
		if(sd->state.lr_flag != 2)
			sd->magic_addrace[type2]+=val;
		break;
	case SP_MAGIC_ADDSIZE:
		if(sd->state.lr_flag != 2)
			sd->magic_addsize[type2]+=val;
		break;
	case SP_ADD_DAMAGE_CLASS:
		switch (sd->state.lr_flag) {
		case 0: //Right hand
			ARR_FIND(0, ARRAYLENGTH(sd->right_weapon.add_dmg), i, sd->right_weapon.add_dmg[i].rate == 0 || sd->right_weapon.add_dmg[i].class_ == type2);
			if (i == ARRAYLENGTH(sd->right_weapon.add_dmg))
			{
				ShowWarning("pc_bonus2: Reached max (%d) number of add Class dmg bonuses per character!\n", ARRAYLENGTH(sd->right_weapon.add_dmg));
				break;
			}
			sd->right_weapon.add_dmg[i].class_ = type2;
			sd->right_weapon.add_dmg[i].rate += val;
			if (!sd->right_weapon.add_dmg[i].rate) //Shift the rest of elements up.
				memmove(&sd->right_weapon.add_dmg[i], &sd->right_weapon.add_dmg[i+1], sizeof(sd->right_weapon.add_dmg) - (i+1)*sizeof(sd->right_weapon.add_dmg[0]));
			break;
		case 1: //Left hand
			ARR_FIND(0, ARRAYLENGTH(sd->left_weapon.add_dmg), i, sd->left_weapon.add_dmg[i].rate == 0 || sd->left_weapon.add_dmg[i].class_ == type2);
			if (i == ARRAYLENGTH(sd->left_weapon.add_dmg))
			{
				ShowWarning("pc_bonus2: Reached max (%d) number of add Class dmg bonuses per character!\n", ARRAYLENGTH(sd->left_weapon.add_dmg));
				break;
			}
			sd->left_weapon.add_dmg[i].class_ = type2;
			sd->left_weapon.add_dmg[i].rate += val;
			if (!sd->left_weapon.add_dmg[i].rate) //Shift the rest of elements up.
				memmove(&sd->left_weapon.add_dmg[i], &sd->left_weapon.add_dmg[i+1], sizeof(sd->left_weapon.add_dmg) - (i+1)*sizeof(sd->left_weapon.add_dmg[0]));
			break;
		}
		break;
	case SP_ADD_MAGIC_DAMAGE_CLASS:
		if(sd->state.lr_flag == 2)
			break;
		ARR_FIND(0, ARRAYLENGTH(sd->add_mdmg), i, sd->add_mdmg[i].rate == 0 || sd->add_mdmg[i].class_ == type2);
		if (i == ARRAYLENGTH(sd->add_mdmg))
		{
			ShowWarning("pc_bonus2: Reached max (%d) number of add Class magic dmg bonuses per character!\n", ARRAYLENGTH(sd->add_mdmg));
			break;
		}
		sd->add_mdmg[i].class_ = type2;
		sd->add_mdmg[i].rate += val;
		if (!sd->add_mdmg[i].rate) //Shift the rest of elements up.
			memmove(&sd->add_mdmg[i], &sd->add_mdmg[i+1], sizeof(sd->add_mdmg) - (i+1)*sizeof(sd->add_mdmg[0]));
		break;
	case SP_ADD_DEF_CLASS:
		if(sd->state.lr_flag == 2)
			break;
		ARR_FIND(0, ARRAYLENGTH(sd->add_def), i, sd->add_def[i].rate == 0 || sd->add_def[i].class_ == type2);
		if (i == ARRAYLENGTH(sd->add_def))
		{
			ShowWarning("pc_bonus2: Reached max (%d) number of add Class def bonuses per character!\n", ARRAYLENGTH(sd->add_def));
			break;
		}
		sd->add_def[i].class_ = type2;
		sd->add_def[i].rate += val;
		if (!sd->add_def[i].rate) //Shift the rest of elements up.
			memmove(&sd->add_def[i], &sd->add_def[i+1], sizeof(sd->add_def) - (i+1)*sizeof(sd->add_def[0]));
		break;
	case SP_ADD_MDEF_CLASS:
		if(sd->state.lr_flag == 2)
			break;
		ARR_FIND(0, ARRAYLENGTH(sd->add_mdef), i, sd->add_mdef[i].rate == 0 || sd->add_mdef[i].class_ == type2);
		if (i == ARRAYLENGTH(sd->add_mdef))
		{
			ShowWarning("pc_bonus2: Reached max (%d) number of add Class mdef bonuses per character!\n", ARRAYLENGTH(sd->add_mdef));
			break;
		}
		sd->add_mdef[i].class_ = type2;
		sd->add_mdef[i].rate += val;
		if (!sd->add_mdef[i].rate) //Shift the rest of elements up.
			memmove(&sd->add_mdef[i], &sd->add_mdef[i+1], sizeof(sd->add_mdef) - (i+1)*sizeof(sd->add_mdef[0]));
		break;
	case SP_HP_DRAIN_RATE:
		if(!sd->state.lr_flag) {
			sd->right_weapon.hp_drain[RC_NONBOSS].rate += type2;
			sd->right_weapon.hp_drain[RC_NONBOSS].per += val;
			sd->right_weapon.hp_drain[RC_BOSS].rate += type2;
			sd->right_weapon.hp_drain[RC_BOSS].per += val;
		}
		else if(sd->state.lr_flag == 1) {
			sd->left_weapon.hp_drain[RC_NONBOSS].rate += type2;
			sd->left_weapon.hp_drain[RC_NONBOSS].per += val;
			sd->left_weapon.hp_drain[RC_BOSS].rate += type2;
			sd->left_weapon.hp_drain[RC_BOSS].per += val;
		}
		break;
	case SP_HP_DRAIN_VALUE:
		if(!sd->state.lr_flag) {
			sd->right_weapon.hp_drain[RC_NONBOSS].value += type2;
			sd->right_weapon.hp_drain[RC_NONBOSS].type = val;
			sd->right_weapon.hp_drain[RC_BOSS].value += type2;
			sd->right_weapon.hp_drain[RC_BOSS].type = val;
		}
		else if(sd->state.lr_flag == 1) {
			sd->left_weapon.hp_drain[RC_NONBOSS].value += type2;
			sd->left_weapon.hp_drain[RC_NONBOSS].type = val;
			sd->left_weapon.hp_drain[RC_BOSS].value += type2;
			sd->left_weapon.hp_drain[RC_BOSS].type = val;
		}
		break;
	case SP_SP_DRAIN_RATE:
		if(!sd->state.lr_flag) {
			sd->right_weapon.sp_drain[RC_NONBOSS].rate += type2;
			sd->right_weapon.sp_drain[RC_NONBOSS].per += val;
			sd->right_weapon.sp_drain[RC_BOSS].rate += type2;
			sd->right_weapon.sp_drain[RC_BOSS].per += val;
		}
		else if(sd->state.lr_flag == 1) {
			sd->left_weapon.sp_drain[RC_NONBOSS].rate += type2;
			sd->left_weapon.sp_drain[RC_NONBOSS].per += val;
			sd->left_weapon.sp_drain[RC_BOSS].rate += type2;
			sd->left_weapon.sp_drain[RC_BOSS].per += val;
		}
		break;
	case SP_SP_DRAIN_VALUE:
		if(!sd->state.lr_flag) {
			sd->right_weapon.sp_drain[RC_NONBOSS].value += type2;
			sd->right_weapon.sp_drain[RC_NONBOSS].type = val;
			sd->right_weapon.sp_drain[RC_BOSS].value += type2;
			sd->right_weapon.sp_drain[RC_BOSS].type = val;
		}
		else if(sd->state.lr_flag == 1) {
			sd->left_weapon.sp_drain[RC_NONBOSS].value += type2;
			sd->left_weapon.sp_drain[RC_NONBOSS].type = val;
			sd->left_weapon.sp_drain[RC_BOSS].value += type2;
			sd->left_weapon.sp_drain[RC_BOSS].type = val;
		}
		break;
	case SP_SP_VANISH_RATE:
		if(sd->state.lr_flag != 2) {
			sd->sp_vanish_rate += type2;
			sd->sp_vanish_per += val;
		}
		break;
	case SP_GET_ZENY_NUM:
		if(sd->state.lr_flag != 2 && sd->get_zeny_rate < val)
		{
			sd->get_zeny_rate = val;
			sd->get_zeny_num = type2;
		}
		break;
	case SP_ADD_GET_ZENY_NUM:
		if(sd->state.lr_flag != 2)
		{
			sd->get_zeny_rate += val;
			sd->get_zeny_num += type2;
		}
		break;
	case SP_WEAPON_COMA_ELE:
		if(type2 >= ELE_MAX) {
			ShowError("pc_bonus2: SP_WEAPON_COMA_ELE: Invalid element %d\n", type2);
			break;
		}
		if(sd->state.lr_flag == 2)
			break;
		sd->weapon_coma_ele[type2] += val;
		sd->special_state.bonus_coma = 1;
		break;
	case SP_WEAPON_COMA_RACE:
		if(sd->state.lr_flag == 2)
			break;
		sd->weapon_coma_race[type2] += val;
		sd->special_state.bonus_coma = 1;
		break;
	case SP_RANDOM_ATTACK_INCREASE:	// [Valaris]
		if(sd->state.lr_flag !=2){
			sd->random_attack_increase_add = type2;
			sd->random_attack_increase_per += val;
		}
		break;
	case SP_WEAPON_ATK:
		if(sd->state.lr_flag != 2)
			sd->weapon_atk[type2]+=val;
		break;
	case SP_WEAPON_ATK_RATE:
		if(sd->state.lr_flag != 2)
			sd->weapon_atk_rate[type2]+=val;
		break;
	case SP_CRITICAL_ADDRACE:
		if(sd->state.lr_flag != 2)
			sd->critaddrace[type2] += val*10;
		break;
	case SP_ADDEFF_WHENHIT:
		if (type2 > SC_MAX) {
			ShowWarning("pc_bonus2 (Add Effect when hit): %d is not supported.\n", type2);
			break;
		}
		if(sd->state.lr_flag != 2)
			pc_bonus_addeff(sd->addeff2, ARRAYLENGTH(sd->addeff2), (sc_type)type2, val, 0, 0);
		break;
	case SP_SKILL_ATK:
		if(sd->state.lr_flag == 2)
			break;
		ARR_FIND(0, ARRAYLENGTH(sd->skillatk), i, sd->skillatk[i].id == 0 || sd->skillatk[i].id == type2);
		if (i == ARRAYLENGTH(sd->skillatk))
		{	//Better mention this so the array length can be updated. [Skotlex]
			ShowDebug("run_script: bonus2 bSkillAtk reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n", ARRAYLENGTH(sd->skillatk), type2, val);
			break;
		}
		if (sd->skillatk[i].id == type2)
			sd->skillatk[i].val += val;
		else {
			sd->skillatk[i].id = type2;
			sd->skillatk[i].val = val;
		}
		break;
	case SP_SKILL_HEAL:
		if(sd->state.lr_flag == 2)
			break;
		ARR_FIND(0, ARRAYLENGTH(sd->skillheal), i, sd->skillheal[i].id == 0 || sd->skillheal[i].id == type2);
		if (i == ARRAYLENGTH(sd->skillheal))
		{ // Better mention this so the array length can be updated. [Skotlex]
			ShowDebug("run_script: bonus2 bSkillHeal reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n", ARRAYLENGTH(sd->skillheal), type2, val);
			break;
		}
		if (sd->skillheal[i].id == type2)
			sd->skillheal[i].val += val;
		else {
			sd->skillheal[i].id = type2;
			sd->skillheal[i].val = val;
		}
		break;
	case SP_SKILL_HEAL2:
		if(sd->state.lr_flag == 2)
			break;
		ARR_FIND(0, ARRAYLENGTH(sd->skillheal2), i, sd->skillheal2[i].id == 0 || sd->skillheal2[i].id == type2);
		if (i == ARRAYLENGTH(sd->skillheal2))
		{ // Better mention this so the array length can be updated. [Skotlex]
			ShowDebug("run_script: bonus2 bSkillHeal2 reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n", ARRAYLENGTH(sd->skillheal2), type2, val);
			break;
		}
		if (sd->skillheal2[i].id == type2)
			sd->skillheal2[i].val += val;
		else {
			sd->skillheal2[i].id = type2;
			sd->skillheal2[i].val = val;
		}
		break;
	case SP_ADD_SKILL_BLOW:
		if(sd->state.lr_flag == 2)
			break;
		ARR_FIND(0, ARRAYLENGTH(sd->skillblown), i, sd->skillblown[i].id == 0 || sd->skillblown[i].id == type2);
		if (i == ARRAYLENGTH(sd->skillblown))
		{	//Better mention this so the array length can be updated. [Skotlex]
			ShowDebug("run_script: bonus2 bSkillBlown reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n", ARRAYLENGTH(sd->skillblown), type2, val);
			break;
		}
		if(sd->skillblown[i].id == type2)
			sd->skillblown[i].val += val;
		else {
			sd->skillblown[i].id = type2;
			sd->skillblown[i].val = val;
		}
		break;

	case SP_CASTRATE:
		if(sd->state.lr_flag == 2)
			break;
		ARR_FIND(0, ARRAYLENGTH(sd->skillcast), i, sd->skillcast[i].id == 0 || sd->skillcast[i].id == type2);
		if (i == ARRAYLENGTH(sd->skillcast))
		{	//Better mention this so the array length can be updated. [Skotlex]
			ShowDebug("run_script: bonus2 bCastRate reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n", ARRAYLENGTH(sd->skillcast), type2, val);
			break;
		}
		if(sd->skillcast[i].id == type2)
			sd->skillcast[i].val += val;
		else {
			sd->skillcast[i].id = type2;
			sd->skillcast[i].val = val;
		}
		break;

	case SP_HP_LOSS_RATE:
		if(sd->state.lr_flag != 2) {
			sd->hp_loss.value = type2;
			sd->hp_loss.rate = val;
		}
		break;
	case SP_HP_REGEN_RATE:
		if(sd->state.lr_flag != 2) {
			sd->hp_regen.value = type2;
			sd->hp_regen.rate = val;
		}
		break;
	case SP_ADDRACE2:
		if (!(type2 > 0 && type2 < MAX_MOB_RACE_DB))
			break;
		if(sd->state.lr_flag != 2)
			sd->right_weapon.addrace2[type2] += val;
		else
			sd->left_weapon.addrace2[type2] += val;
		break;
	case SP_SUBSIZE:
		if(sd->state.lr_flag != 2)
			sd->subsize[type2]+=val;
		break;
	case SP_SUBRACE2:
		if(sd->state.lr_flag != 2)
			sd->subrace2[type2]+=val;
		break;
	case SP_ADD_ITEM_HEAL_RATE:
		if(sd->state.lr_flag == 2)
			break;
		if (type2 < MAX_ITEMGROUP) {	//Group bonus
			sd->itemgrouphealrate[type2] += val;
			break;
		}
		//Standard item bonus.
		for(i=0; i < ARRAYLENGTH(sd->itemhealrate) && sd->itemhealrate[i].nameid && sd->itemhealrate[i].nameid != type2; i++);
		if(i == ARRAYLENGTH(sd->itemhealrate)) {
			ShowWarning("pc_bonus2: Reached max (%d) number of item heal bonuses per character!\n", ARRAYLENGTH(sd->itemhealrate));
			break;
		}
		sd->itemhealrate[i].nameid = type2;
		sd->itemhealrate[i].rate += val;
		break;
	case SP_EXP_ADDRACE:
		if(sd->state.lr_flag != 2)
			sd->expaddrace[type2]+=val;
		break;
	case SP_SP_GAIN_RACE:
		if(sd->state.lr_flag != 2)
			sd->sp_gain_race[type2]+=val;
		break;
	case SP_ADD_MONSTER_DROP_ITEM:
		if (sd->state.lr_flag != 2)
			pc_bonus_item_drop(sd->add_drop, ARRAYLENGTH(sd->add_drop), type2, 0, (1<<RC_BOSS)|(1<<RC_NONBOSS), val);
		break;
	case SP_ADD_MONSTER_DROP_ITEMGROUP:
		if (sd->state.lr_flag != 2)
			pc_bonus_item_drop(sd->add_drop, ARRAYLENGTH(sd->add_drop), 0, type2, (1<<RC_BOSS)|(1<<RC_NONBOSS), val);
		break;
	case SP_SP_LOSS_RATE:
		if(sd->state.lr_flag != 2) {
			sd->sp_loss.value = type2;
			sd->sp_loss.rate = val;
		}
		break;
	case SP_SP_REGEN_RATE:
		if(sd->state.lr_flag != 2) {
			sd->sp_regen.value = type2;
			sd->sp_regen.rate = val;
		}
		break;
	case SP_HP_DRAIN_VALUE_RACE:
		if(!sd->state.lr_flag) {
			sd->right_weapon.hp_drain[type2].value += val;
		}
		else if(sd->state.lr_flag == 1) {
			sd->left_weapon.hp_drain[type2].value += val;
		}
		break;
	case SP_SP_DRAIN_VALUE_RACE:
		if(!sd->state.lr_flag) {
			sd->right_weapon.sp_drain[type2].value += val;
		}
		else if(sd->state.lr_flag == 1) {
			sd->left_weapon.sp_drain[type2].value += val;
		}
		break;
	case SP_IGNORE_MDEF_RATE:
		if(sd->state.lr_flag != 2)
			sd->ignore_mdef[type2] += val;
		break;
	case SP_IGNORE_DEF_RATE:
		if(sd->state.lr_flag != 2)
			sd->ignore_def[type2] += val;
		break;

	default:
		ShowWarning("pc_bonus2: unknown type %d %d %d!\n",type,type2,val);
		break;
	}
	return 0;
}

int pc_bonus3(struct map_session_data *sd,int type,int type2,int type3,int val)
{
	nullpo_retr(0, sd);

	switch(type){
	case SP_ADD_MONSTER_DROP_ITEM:
		if(sd->state.lr_flag != 2)
			pc_bonus_item_drop(sd->add_drop, ARRAYLENGTH(sd->add_drop), type2, 0, 1<<type3, val);
		break;
	case SP_AUTOSPELL:
		if(sd->state.lr_flag != 2)
		{
			int target = skill_get_inf(type2); //Support or Self (non-auto-target) skills should pick self.
			target = target&INF_SUPPORT_SKILL || (target&INF_SELF_SKILL && !(skill_get_inf2(type2)&INF2_NO_TARGET_SELF));
			pc_bonus_autospell(sd->autospell, ARRAYLENGTH(sd->autospell),
				target?-type2:type2, type3, val, 0, current_equip_card_id);
		}
		break;
	case SP_AUTOSPELL_WHENHIT:
		if(sd->state.lr_flag != 2)
		{
			int target = skill_get_inf(type2); //Support or Self (non-auto-target) skills should pick self.
			target = target&INF_SUPPORT_SKILL || (target&INF_SELF_SKILL && !(skill_get_inf2(type2)&INF2_NO_TARGET_SELF));
			pc_bonus_autospell(sd->autospell2, ARRAYLENGTH(sd->autospell2),
				target?-type2:type2, type3, val, BF_NORMAL|BF_SKILL, current_equip_card_id);
		}
		break;
	case SP_SP_DRAIN_RATE:
		if(!sd->state.lr_flag) {
			sd->right_weapon.sp_drain[RC_NONBOSS].rate += type2;
			sd->right_weapon.sp_drain[RC_NONBOSS].per += type3;
			sd->right_weapon.sp_drain[RC_NONBOSS].type = val;
			sd->right_weapon.sp_drain[RC_BOSS].rate += type2;
			sd->right_weapon.sp_drain[RC_BOSS].per += type3;
			sd->right_weapon.sp_drain[RC_BOSS].type = val;

		}
		else if(sd->state.lr_flag == 1) {
			sd->left_weapon.sp_drain[RC_NONBOSS].rate += type2;
			sd->left_weapon.sp_drain[RC_NONBOSS].per += type3;
			sd->left_weapon.sp_drain[RC_NONBOSS].type = val;
			sd->left_weapon.sp_drain[RC_BOSS].rate += type2;
			sd->left_weapon.sp_drain[RC_BOSS].per += type3;
			sd->left_weapon.sp_drain[RC_BOSS].type = val;
		}
		break;
	case SP_HP_DRAIN_RATE_RACE:
		if(!sd->state.lr_flag) {
			sd->right_weapon.hp_drain[type2].rate += type3;
			sd->right_weapon.hp_drain[type2].per += val;
		}
		else if(sd->state.lr_flag == 1) {
			sd->left_weapon.hp_drain[type2].rate += type3;
			sd->left_weapon.hp_drain[type2].per += val;
		}
		break;
	case SP_SP_DRAIN_RATE_RACE:
		if(!sd->state.lr_flag) {
			sd->right_weapon.sp_drain[type2].rate += type3;
			sd->right_weapon.sp_drain[type2].per += val;
		}
		else if(sd->state.lr_flag == 1) {
			sd->left_weapon.sp_drain[type2].rate += type3;
			sd->left_weapon.sp_drain[type2].per += val;
		}
		break;
	case SP_ADD_MONSTER_DROP_ITEMGROUP:
		if (sd->state.lr_flag != 2)
			pc_bonus_item_drop(sd->add_drop, ARRAYLENGTH(sd->add_drop), 0, type2, 1<<type3, val);
		break;

	case SP_ADDEFF:
		if (type2 > SC_MAX) {
			ShowWarning("pc_bonus3 (Add Effect): %d is not supported.\n", type2);
			break;
		}
		pc_bonus_addeff(sd->addeff, ARRAYLENGTH(sd->addeff), (sc_type)type2,
			sd->state.lr_flag!=2?type3:0, sd->state.lr_flag==2?type3:0, val);
		break;

	case SP_ADDEFF_WHENHIT:
		if (type2 > SC_MAX) {
			ShowWarning("pc_bonus3 (Add Effect when hit): %d is not supported.\n", type2);
			break;
		}
		if(sd->state.lr_flag != 2)
			pc_bonus_addeff(sd->addeff2, ARRAYLENGTH(sd->addeff2), (sc_type)type2, type3, 0, val);
		break;

	case SP_ADDEFF_ONSKILL:
		if( type3 > SC_MAX ) {
			ShowWarning("pc_bonus3 (Add Effect on skill): %d is not supported.\n", type3);
			break;
		}
		if( sd->state.lr_flag != 2 )
			pc_bonus_addeff_onskill(sd->addeff3, ARRAYLENGTH(sd->addeff3), (sc_type)type3, val, type2, 2);
		break;

	default:
		ShowWarning("pc_bonus3: unknown type %d %d %d %d!\n",type,type2,type3,val);
		break;
	}

	return 0;
}

int pc_bonus4(struct map_session_data *sd,int type,int type2,int type3,int type4,int val)
{
	nullpo_retr(0, sd);

	switch(type){
	case SP_AUTOSPELL:
		if(sd->state.lr_flag != 2)
			pc_bonus_autospell(sd->autospell, ARRAYLENGTH(sd->autospell), (val&1?type2:-type2), (val&2?-type3:type3), type4, 0, current_equip_card_id);
		break;

	case SP_AUTOSPELL_WHENHIT:
		if(sd->state.lr_flag != 2)
			pc_bonus_autospell(sd->autospell2, ARRAYLENGTH(sd->autospell2), (val&1?type2:-type2), (val&2?-type3:type3), type4, BF_NORMAL|BF_SKILL, current_equip_card_id);
		break;

	case SP_AUTOSPELL_ONSKILL:
		if(sd->state.lr_flag != 2)
		{
			int target = skill_get_inf(type2); //Support or Self (non-auto-target) skills should pick self.
			target = target&INF_SUPPORT_SKILL || (target&INF_SELF_SKILL && !(skill_get_inf2(type2)&INF2_NO_TARGET_SELF));

			pc_bonus_autospell_onskill(sd->autospell3, ARRAYLENGTH(sd->autospell3), type2, target?-type3:type3, type4, val, current_equip_card_id);
		}
		break;

	case SP_ADDEFF_ONSKILL:
		if( type2 > SC_MAX ) {
			ShowWarning("pc_bonus3 (Add Effect on skill): %d is not supported.\n", type2);
			break;
		}
		if( sd->state.lr_flag != 2 )
			pc_bonus_addeff_onskill(sd->addeff3, ARRAYLENGTH(sd->addeff3), (sc_type)type3, type4, type2, val);
		break;

	default:
		ShowWarning("pc_bonus4: unknown type %d %d %d %d %d!\n",type,type2,type3,type4,val);
		break;
	}

	return 0;
}

int pc_bonus5(struct map_session_data *sd,int type,int type2,int type3,int type4,int type5,int val)
{
	nullpo_retr(0, sd);

	switch(type){
	case SP_AUTOSPELL:
		if(sd->state.lr_flag != 2)
			pc_bonus_autospell(sd->autospell, ARRAYLENGTH(sd->autospell), (val&1?type2:-type2), (val&2?-type3:type3), type4, type5, current_equip_card_id);
		break;

	case SP_AUTOSPELL_WHENHIT:
		if(sd->state.lr_flag != 2)
			pc_bonus_autospell(sd->autospell2, ARRAYLENGTH(sd->autospell2), (val&1?type2:-type2), (val&2?-type3:type3), type4, type5, current_equip_card_id);
		break;

	case SP_AUTOSPELL_ONSKILL:
		if(sd->state.lr_flag != 2)
			pc_bonus_autospell_onskill(sd->autospell3, ARRAYLENGTH(sd->autospell3), type2, (val&1?-type3:type3), (val&2?-type4:type4), type5, current_equip_card_id);
		break;

	default:
		ShowWarning("pc_bonus5: unknown type %d %d %d %d %d %d!\n",type,type2,type3,type4,type5,val);
		break;
	}

	return 0;
}

/*==========================================
 *	Grants a player a given skill. Flag values are:
 *	0 - Grant skill unconditionally and forever (only this one invokes status_calc_pc,
 *	    as the other two are assumed to be invoked from within it)
 *	1 - Grant an item skill (temporary)
 *	2 - Like 1, except the level granted can stack with previously learned level.
 *------------------------------------------*/
int pc_skill(TBL_PC* sd, int id, int level, int flag)
{
	nullpo_retr(0, sd);

	if( id <= 0 || id >= MAX_SKILL || skill_db[id].name == NULL) {
		ShowError("pc_skill: Skill with id %d does not exist in the skill database\n", id);
		return 0;
	}
	if( level > MAX_SKILL_LEVEL ) {
		ShowError("pc_skill: Skill level %d too high. Max lv supported is %d\n", level, MAX_SKILL_LEVEL);
		return 0;
	}
	if( flag == 2 && sd->status.skill[id].lv + level > MAX_SKILL_LEVEL ) {
		ShowError("pc_skill: Skill level bonus %d too high. Max lv supported is %d. Curr lv is %d\n", level, MAX_SKILL_LEVEL, sd->status.skill[id].lv);
		return 0;
	}

	switch( flag ){
	case 0: //Set skill data overwriting whatever was there before.
		sd->status.skill[id].id   = id;
		sd->status.skill[id].lv   = level;
		sd->status.skill[id].flag = 0;
		if( !level ) //Remove skill.
		{
			sd->status.skill[id].id = 0;
			clif_deleteskill(sd,id);
		}
		else
			clif_addskill(sd,id);
		if( !skill_get_inf(id) ) //Only recalculate for passive skills.
			status_calc_pc(sd, 0);
	break;
	case 1: //Item bonus skill.
		if( sd->status.skill[id].id == id ){
			if( sd->status.skill[id].lv >= level )
				return 0;
			if( !sd->status.skill[id].flag ) //Non-granted skill, store it's level.
				sd->status.skill[id].flag = sd->status.skill[id].lv + 2;
		} else {
			sd->status.skill[id].id   = id;
			sd->status.skill[id].flag = 1;
		}
		sd->status.skill[id].lv = level;
	break;
	case 2: //Add skill bonus on top of what you had.
		if( sd->status.skill[id].id == id ){
			if( !sd->status.skill[id].flag ) // Store previous level.
				sd->status.skill[id].flag = sd->status.skill[id].lv + 2;
		} else {
			sd->status.skill[id].id   = id;
			sd->status.skill[id].flag = 1; //Set that this is a bonus skill.
		}
		sd->status.skill[id].lv += level;
	break;
	default: //Unknown flag?
		return 0;
	}
	return 1;
}
/*==========================================
 * J?h?ü
 *------------------------------------------*/
int pc_insert_card(struct map_session_data* sd, int idx_card, int idx_equip)
{
	int i;
	int nameid;

	nullpo_retr(0, sd);

	if( idx_equip < 0 || idx_equip >= MAX_INVENTORY || sd->inventory_data[idx_equip] == NULL )
		return 0; //Invalid item index.
	if( idx_card < 0 || idx_card >= MAX_INVENTORY || sd->inventory_data[idx_card] == NULL )
		return 0; //Invalid card index.
	if( sd->status.inventory[idx_equip].nameid <= 0 || sd->status.inventory[idx_equip].amount < 1 )
		return 0; // target item missing
	if( sd->status.inventory[idx_card].nameid <= 0 || sd->status.inventory[idx_card].amount < 1 )
		return 0; // target card missing
	if( sd->inventory_data[idx_equip]->type != IT_WEAPON && sd->inventory_data[idx_equip]->type != IT_ARMOR )
		return 0; // only weapons and armor are allowed
	if( sd->inventory_data[idx_card]->type != IT_CARD )
		return 0; // must be a card
	if( sd->status.inventory[idx_equip].identify == 0 )
		return 0; // target must be identified
	if( itemdb_isspecial(sd->status.inventory[idx_equip].card[0]) )
		return 0; // card slots reserved for other purposes
	if( (sd->inventory_data[idx_equip]->equip & sd->inventory_data[idx_card]->equip) == 0 )
		return 0; // card cannot be compounded on this item type
	if( sd->inventory_data[idx_equip]->type == IT_WEAPON && sd->inventory_data[idx_card]->equip == EQP_SHIELD )
		return 0; // attempted to place shield card on left-hand weapon.
	if( sd->status.inventory[idx_equip].equip != 0 )
		return 0; // item must be unequipped

	ARR_FIND( 0, sd->inventory_data[idx_equip]->slot, i, sd->status.inventory[idx_equip].card[i] == 0 );
	if( i == sd->inventory_data[idx_equip]->slot )
		return 0; // no free slots

	// remember the card id to insert
	nameid = sd->status.inventory[idx_card].nameid;

	if( pc_delitem(sd,idx_card,1,1) == 1 )
	{// failed
		clif_insert_card(sd,idx_equip,idx_card,1);
	}
	else
	{// success
		sd->status.inventory[idx_equip].card[i] = nameid;
		clif_insert_card(sd,idx_equip,idx_card,0);
	}

	return 0;
}

//
// ACe¨
//

/*==========================================
 * XLÉæé¢lC³
 *------------------------------------------*/
int pc_modifybuyvalue(struct map_session_data *sd,int orig_value)
{
	int skill,val = orig_value,rate1 = 0,rate2 = 0;
	if((skill=pc_checkskill(sd,MC_DISCOUNT))>0)	// fBXJEg
		rate1 = 5+skill*2-((skill==10)? 1:0);
	if((skill=pc_checkskill(sd,RG_COMPULSION))>0)	// RpVfBXJEg
		rate2 = 5+skill*4;
	if(rate1 < rate2) rate1 = rate2;
	if(rate1)
		val = (int)((double)orig_value*(double)(100-rate1)/100.);
	if(val < 0) val = 0;
	if(orig_value > 0 && val < 1) val = 1;

	return val;
}

/*==========================================
 * XLÉæé?èlC³
 *------------------------------------------*/
int pc_modifysellvalue(struct map_session_data *sd,int orig_value)
{
	int skill,val = orig_value,rate = 0;
	if((skill=pc_checkskill(sd,MC_OVERCHARGE))>0)	// I?o?`?W
		rate = 5+skill*2-((skill==10)? 1:0);
	if(rate)
		val = (int)((double)orig_value*(double)(100+rate)/100.);
	if(val < 0) val = 0;
	if(orig_value > 0 && val < 1) val = 1;

	return val;
}

/*==========================================
 * ACeðÁ½bÉAVµ¢ACeðg¤©A
 * 3Â§ÀÉ©©é©mF
 *------------------------------------------*/
int pc_checkadditem(struct map_session_data *sd,int nameid,int amount)
{
	int i;

	nullpo_retr(0, sd);

	if(!itemdb_isstackable(nameid))
		return ADDITEM_NEW;

	for(i=0;i<MAX_INVENTORY;i++){
		if(sd->status.inventory[i].nameid==nameid){
			if(sd->status.inventory[i].amount+amount > MAX_AMOUNT)
				return ADDITEM_OVERAMOUNT;
			return ADDITEM_EXIST;
		}
	}

	if(amount > MAX_AMOUNT)
		return ADDITEM_OVERAMOUNT;
	return ADDITEM_NEW;
}

/*==========================================
 * ó«ACeÌÂ?
 *------------------------------------------*/
int pc_inventoryblank(struct map_session_data *sd)
{
	int i,b;

	nullpo_retr(0, sd);

	for(i=0,b=0;i<MAX_INVENTORY;i++){
		if(sd->status.inventory[i].nameid==0)
			b++;
	}

	return b;
}

/*==========================================
 * ¨àð?¤
 *------------------------------------------*/
int pc_payzeny(struct map_session_data *sd,int zeny)
{
	nullpo_retr(0, sd);

	if( zeny < 0 )
	  	return pc_getzeny(sd, -zeny);

	if( sd->status.zeny < zeny )
		return 1; //Not enough.

	sd->status.zeny -= zeny;
	clif_updatestatus(sd,SP_ZENY);

	return 0;
}
/*==========================================
 * Cash Shop
 *------------------------------------------*/

void pc_paycash(struct map_session_data *sd, int price, int points)
{
	char output[128];
	int cash = price - points;
	nullpo_retv(sd);

	pc_setaccountreg(sd,"#CASHPOINTS",sd->cashPoints - cash);
	pc_setaccountreg(sd,"#KAFRAPOINTS",sd->kafraPoints - points);
	sprintf(output, "Used %d kafra points and %d cash points. %d kafra and %d cash points remaining.", points, cash, sd->kafraPoints, sd->cashPoints);
	clif_disp_onlyself(sd, output, strlen(output));
}

void pc_getcash(struct map_session_data *sd, int cash, int points)
{
	char output[128];
	nullpo_retv(sd);

	if( cash > 0 )
	{
		pc_setaccountreg(sd,"#CASHPOINTS",sd->cashPoints + cash);

		sprintf(output, "Gained %d cash points. Total %d points", cash, sd->cashPoints);
		clif_disp_onlyself(sd, output, strlen(output));
	}

	if( points > 0 )
	{
		pc_setaccountreg(sd,"#KAFRAPOINTS",sd->kafraPoints + points);

		sprintf(output, "Gained %d kafra points. Total %d points", points, sd->kafraPoints);
		clif_disp_onlyself(sd, output, strlen(output));
	}
}

/*==========================================
 * ¨àð¾é
 *------------------------------------------*/
int pc_getzeny(struct map_session_data *sd,int zeny)
{
	nullpo_retr(0, sd);

	if( zeny < 0 )
		return pc_payzeny(sd, -zeny);

	if( zeny > MAX_ZENY - sd->status.zeny )
		zeny = MAX_ZENY - sd->status.zeny;

	sd->status.zeny += zeny;
	clif_updatestatus(sd,SP_ZENY);

	if( zeny > 0 && sd->state.showzeny )
	{
		char output[255];
		sprintf(output, "Gained %dz.", zeny);
		clif_disp_onlyself(sd,output,strlen(output));
	}

	return 0;
}

/*==========================================
 * ACeðTµÄACfbNXðÔ·
 *------------------------------------------*/
int pc_search_inventory(struct map_session_data *sd,int item_id)
{
	int i;
	nullpo_retr(-1, sd);

	ARR_FIND( 0, MAX_INVENTORY, i, sd->status.inventory[i].nameid == item_id && (sd->status.inventory[i].amount > 0 || item_id == 0) );
	return ( i < MAX_INVENTORY ) ? i : -1;
}

/*==========================================
 * ACeÇÁBÂ?ÌÝitem\¢?Ì?ð³
 *------------------------------------------*/
int pc_additem(struct map_session_data *sd,struct item *item_data,int amount)
{
	struct item_data *data;
	int i;
	unsigned int w;

	nullpo_retr(1, sd);
	nullpo_retr(1, item_data);

	if( item_data->nameid <= 0 || amount <= 0 )
		return 1;
	if( amount > MAX_AMOUNT )
		return 5;
	
	data = itemdb_search(item_data->nameid);
	w = data->weight*amount;
	if(sd->weight + w > sd->max_weight)
		return 2;

	i = MAX_INVENTORY;

	if( itemdb_isstackable2(data) && item_data->expire_time == 0 )
	{ // Stackable | Non Rental
		for( i = 0; i < MAX_INVENTORY; i++ )
		{
			if( sd->status.inventory[i].nameid == item_data->nameid && memcmp(&sd->status.inventory[i].card, &item_data->card, sizeof(item_data->card)) == 0 )
			{
				if( amount > MAX_AMOUNT - sd->status.inventory[i].amount )
					return 5;
				sd->status.inventory[i].amount += amount;
				clif_additem(sd,i,amount,0);
				break;
			}
		}
	}

	if( i >= MAX_INVENTORY )
	{
		i = pc_search_inventory(sd,0);
		if( i < 0 )
			return 4;

		memcpy(&sd->status.inventory[i], item_data, sizeof(sd->status.inventory[0]));
		// clear equips field first, just in case
		if( item_data->equip )
			sd->status.inventory[i].equip = 0;

		sd->status.inventory[i].amount = amount;
		sd->inventory_data[i] = data;
		clif_additem(sd,i,amount,0);
	}

	sd->weight += w;
	clif_updatestatus(sd,SP_WEIGHT);
	//Auto-equip
	if(data->flag.autoequip) pc_equipitem(sd, i, data->equip);
	return 0;
}

/*==========================================
 * ACeð¸ç·
 *------------------------------------------*/
int pc_delitem(struct map_session_data *sd,int n,int amount,int type)
{
	nullpo_retr(1, sd);

	if(sd->status.inventory[n].nameid==0 || amount <= 0 || sd->status.inventory[n].amount<amount || sd->inventory_data[n] == NULL)
		return 1;

	sd->status.inventory[n].amount -= amount;
	sd->weight -= sd->inventory_data[n]->weight*amount ;
	if(sd->status.inventory[n].amount<=0){
		if(sd->status.inventory[n].equip)
			pc_unequipitem(sd,n,3);
		memset(&sd->status.inventory[n],0,sizeof(sd->status.inventory[0]));
		sd->inventory_data[n] = NULL;
	}
	if(!(type&1))
		clif_delitem(sd,n,amount);
	if(!(type&2))
		clif_updatestatus(sd,SP_WEIGHT);

	return 0;
}

/*==========================================
 * ACeð·
 *------------------------------------------*/
int pc_dropitem(struct map_session_data *sd,int n,int amount)
{
	nullpo_retr(1, sd);

	if(n < 0 || n >= MAX_INVENTORY)
		return 0;

	if(amount <= 0)
		return 0;

	if(sd->status.inventory[n].nameid <= 0 ||
		sd->status.inventory[n].amount <= 0 ||
		sd->status.inventory[n].amount < amount ||
		sd->state.trading || sd->vender_id != 0 ||
		!sd->inventory_data[n] //pc_delitem would fail on this case.
		)
		return 0;

	if( map[sd->bl.m].flag.nodrop )
	{
		clif_displaymessage (sd->fd, msg_txt(271));
		return 0; //Can't drop items in nodrop mapflag maps.
	}
	
	if( !pc_candrop(sd,&sd->status.inventory[n]) )
	{
		clif_displaymessage (sd->fd, msg_txt(263));
		return 0;
	}
	
	//Logs items, dropped by (P)layers [Lupus]
	if(log_config.enable_logs&0x8)
		log_pick_pc(sd, "P", sd->status.inventory[n].nameid, -amount, (struct item*)&sd->status.inventory[n]);
	//Logs

	if (!map_addflooritem(&sd->status.inventory[n], amount, sd->bl.m, sd->bl.x, sd->bl.y, 0, 0, 0, 2))
		return 0;
	
	pc_delitem(sd, n, amount, 0);
	return 1;
}

/*==========================================
 * ACeðE¤
 *------------------------------------------*/
int pc_takeitem(struct map_session_data *sd,struct flooritem_data *fitem)
{
	int flag=0;
	unsigned int tick = gettick();
	struct map_session_data *first_sd = NULL,*second_sd = NULL,*third_sd = NULL;
	struct party_data *p=NULL;

	nullpo_retr(0, sd);
	nullpo_retr(0, fitem);

	if(!check_distance_bl(&fitem->bl, &sd->bl, 2) && sd->ud.skillid!=BS_GREED)
		return 0;	// £ª¢

	if (sd->status.party_id)
		p = party_search(sd->status.party_id);
	
	if(fitem->first_get_charid > 0 && fitem->first_get_charid != sd->status.char_id)
  	{
		first_sd = map_charid2sd(fitem->first_get_charid);
		if(DIFF_TICK(tick,fitem->first_get_tick) < 0) {
			if (!(p && p->party.item&1 &&
				first_sd && first_sd->status.party_id == sd->status.party_id
			))
				return 0;
		}
		else
		if(fitem->second_get_charid > 0 && fitem->second_get_charid != sd->status.char_id)
	  	{
			second_sd = map_charid2sd(fitem->second_get_charid);
			if(DIFF_TICK(tick, fitem->second_get_tick) < 0) {
				if(!(p && p->party.item&1 &&
					((first_sd && first_sd->status.party_id == sd->status.party_id) ||
					(second_sd && second_sd->status.party_id == sd->status.party_id))
				))
					return 0;
			}
			else
			if(fitem->third_get_charid > 0 && fitem->third_get_charid != sd->status.char_id)
		  	{
				third_sd = map_charid2sd(fitem->third_get_charid);
				if(DIFF_TICK(tick,fitem->third_get_tick) < 0) {
					if(!(p && p->party.item&1 &&
						((first_sd && first_sd->status.party_id == sd->status.party_id) ||
						(second_sd && second_sd->status.party_id == sd->status.party_id) ||
						(third_sd && third_sd->status.party_id == sd->status.party_id))
					))
						return 0;
				}
			}
		}
	}

	//This function takes care of giving the item to whoever should have it, considering party-share options.
	if ((flag = party_share_loot(p,sd,&fitem->item_data, fitem->first_get_charid))) {
		clif_additem(sd,0,0,flag);
		return 1;
	}

	//Display pickup animation.
	pc_stop_attack(sd);
	clif_takeitem(&sd->bl,&fitem->bl);
	map_clearflooritem(fitem->bl.id);
	return 1;
}

int pc_isUseitem(struct map_session_data *sd,int n)
{
	struct item_data *item;
	int nameid;

	nullpo_retr(0, sd);

	item = sd->inventory_data[n];
	nameid = sd->status.inventory[n].nameid;

	if( item == NULL )
		return 0;
	//Not consumable item
	if( item->type != IT_HEALING && item->type != IT_USABLE )
		return 0;
	if( !item->script ) //if it has no script, you can't really consume it!
		return 0;

	switch( nameid )
	{
		case 605: // Anodyne
			if( map_flag_gvg(sd->bl.m) )
				return 0;
		case 606:
			if( pc_issit(sd) )
				return 0;
			break;
		case 601: // Fly Wing
		case 12212: // Giant Fly Wing
			if( map[sd->bl.m].flag.noteleport || map_flag_gvg(sd->bl.m) )
			{
				clif_skill_teleportmessage(sd,0);
				return 0;
			}
		case 602: // ButterFly Wing
		case 14527: // Dungeon Teleport Scroll
		case 14581: // Dungeon Teleport Scroll
		case 14582: // Yellow Butterfly Wing
		case 14583: // Green Butterfly Wing
		case 14584: // Red Butterfly Wing
		case 14585: // Blue Butterfly Wing
		case 14591: // Siege Teleport Scroll
			if( sd->duel_group && !battle_config.duel_allow_teleport )
			{
				clif_displaymessage(sd->fd, "Duel: Can't use this item in duel.");
				return 0;
			}
			if( nameid != 601 && nameid != 12212 && map[sd->bl.m].flag.noreturn )
				return 0;
			break;
		case 604: // Dead Branch
		case 12024: // Red Pouch
		case 12103: // Bloody Branch
		case 12109: // Poring Box
			if( map[sd->bl.m].flag.nobranch || map_flag_gvg(sd->bl.m) )
				return 0;
			break;
		case 12210: // Bubble Gum
		case 12264: // Comp Bubble Gum
			if( sd->sc.data[SC_ITEMBOOST] )
				return 0;
			break;
		case 12208: // Battle Manual
		case 12263: // Comp Battle Manual
		case 12312: // Thick Battle Manual
		case 12705: // Noble Nameplate
		case 14532: // Battle_Manual25
		case 14533: // Battle_Manual100
		case 14545: // Battle_Manual300
			if( sd->sc.data[SC_EXPBOOST] )
				return 0;
			break;
		case 14592: // JOB_Battle_Manual
			if( sd->sc.data[SC_JEXPBOOST] )
				return 0;
			break;

		// Mercenary Items

		case 12184: // Mercenary's Red Potion
		case 12185: // Mercenary's Blue Potion
		case 12241: // Mercenary's Concentration Potion
		case 12242: // Mercenary's Awakening Potion
		case 12243: // Mercenary's Berserk Potion
			if( sd->md == NULL || sd->md->db == NULL )
				return 0;
			if( sd->md->sc.data[SC_BERSERK] )
				return 0;
			if( nameid == 12242 && sd->md->db->lv < 40 )
				return 0;
			if( nameid == 12243 && sd->md->db->lv < 80 )
				return 0;
			break;
	}

	if( nameid >= 12153 && nameid <= 12182 && sd->md != NULL )
		return 0; // Mercenary Scrolls

	//added item_noequip.txt items check by Maya&[Lupus]
	if (
		(map[sd->bl.m].flag.pvp && item->flag.no_equip&1) || // PVP
		(map_flag_gvg(sd->bl.m) && item->flag.no_equip&2) || // GVG
		(map[sd->bl.m].flag.restricted && item->flag.no_equip&map[sd->bl.m].zone) // Zone restriction
	)
		return 0;

	//Gender check
	if(item->sex != 2 && sd->status.sex != item->sex)
		return 0;
	//Required level check
	if(item->elv && sd->status.base_level < (unsigned int)item->elv)
		return 0;

	//Not equipable by class. [Skotlex]
	if (!(
		(1<<(sd->class_&MAPID_BASEMASK)) &
		(item->class_base[sd->class_&JOBL_2_1?1:(sd->class_&JOBL_2_2?2:0)])
	))
		return 0;
	
	//Not usable by upper class. [Skotlex]
	if(!(
		(1<<(sd->class_&JOBL_UPPER?1:(sd->class_&JOBL_BABY?2:0))) &
		item->class_upper
	))
		return 0;

	//Dead Branch & Bloody Branch & Porings Box
	if((log_config.branch > 0) && (nameid == 604 || nameid == 12103 || nameid == 12109))
		log_branch(sd);

	return 1;
}

/*==========================================
 * ACeðg¤
 *------------------------------------------*/
int pc_useitem(struct map_session_data *sd,int n)
{
	unsigned int tick = gettick();
	int amount;
	struct script_code *script;

	nullpo_retr(0, sd);

	if( sd->status.inventory[n].nameid <= 0 || sd->status.inventory[n].amount <= 0 )
		return 0;

	if( !pc_isUseitem(sd,n) )
		return 0;

	 //Prevent mass item usage. [Skotlex]
	if( DIFF_TICK(sd->canuseitem_tick, tick) > 0 )
		return 0;

	if( sd->sc.count && (
		sd->sc.data[SC_BERSERK] ||
		(sd->sc.data[SC_GRAVITATION] && sd->sc.data[SC_GRAVITATION]->val3 == BCT_SELF) ||
		sd->sc.data[SC_TRICKDEAD] ||
		sd->sc.data[SC_HIDING] ||
		(sd->sc.data[SC_NOCHAT] && sd->sc.data[SC_NOCHAT]->val1&MANNER_NOITEM)
	))
		return 0;

	//Since most delay-consume items involve using a "skill-type" target cursor,
	//perform a skill-use check before going through. [Skotlex]
	//resurrection was picked as testing skill, as a non-offensive, generic skill, it will do.
	//FIXME: Is this really needed here? It'll be checked in unit.c after all and this prevents skill items using when silenced [Inkfish]
	if( sd->inventory_data[n]->flag.delay_consume && ( sd->ud.skilltimer != -1 /*|| !status_check_skilluse(&sd->bl, &sd->bl, ALL_RESURRECTION, 0)*/ ) )
		return 0;

	sd->itemid = sd->status.inventory[n].nameid;
	sd->itemindex = n;
	if(sd->catch_target_class != -1) //Abort pet catching.
		sd->catch_target_class = -1;

	amount = sd->status.inventory[n].amount;
	script = sd->inventory_data[n]->script;
	//Check if the item is to be consumed immediately [Skotlex]
	if( sd->inventory_data[n]->flag.delay_consume )
		clif_useitemack(sd,n,amount,1);
	else
	{
		if( sd->status.inventory[n].expire_time == 0 )
		{
			clif_useitemack(sd,n,amount-1,1);

			//Logs (C)onsumable items [Lupus]
			if( log_config.enable_logs&0x100 )
				log_pick_pc(sd, "C", sd->status.inventory[n].nameid, -1, &sd->status.inventory[n]);

			pc_delitem(sd,n,1,1); // Rental Usable Items are not deleted until expiration
		}
		else
			clif_useitemack(sd,n,0,0);
	}
	if(sd->status.inventory[n].card[0]==CARD0_CREATE &&
		pc_famerank(MakeDWord(sd->status.inventory[n].card[2],sd->status.inventory[n].card[3]), MAPID_ALCHEMIST))
	{
	    potion_flag = 2; // Famous player's potions have 50% more efficiency
		 if (sd->sc.data[SC_SPIRIT] && sd->sc.data[SC_SPIRIT]->val2 == SL_ROGUE)
			 potion_flag = 3; //Even more effective potions.
	}

	sd->canuseitem_tick= tick + battle_config.item_use_interval; //Update item use time.
	run_script(script,0,sd->bl.id,fake_nd->bl.id);
	potion_flag = 0;
	return 1;
}

/*==========================================
 * J?gACeÇÁBÂ?ÌÝitem\¢?Ì?ð³
 *------------------------------------------*/
int pc_cart_additem(struct map_session_data *sd,struct item *item_data,int amount)
{
	struct item_data *data;
	int i,w;

	nullpo_retr(1, sd);
	nullpo_retr(1, item_data);

	if(item_data->nameid <= 0 || amount <= 0)
		return 1;
	data = itemdb_search(item_data->nameid);

	if( !itemdb_cancartstore(item_data, pc_isGM(sd)) )
	{ // Check item trade restrictions	[Skotlex]
		clif_displaymessage (sd->fd, msg_txt(264));
		return 1;
	}

	if( (w = data->weight*amount) + sd->cart_weight > battle_config.max_cart_weight )
		return 1;

	i = MAX_CART;
	if( itemdb_isstackable2(data) && !item_data->expire_time )
	{
		ARR_FIND( 0, MAX_CART, i,
			sd->status.cart[i].nameid == item_data->nameid &&
			sd->status.cart[i].card[0] == item_data->card[0] && sd->status.cart[i].card[1] == item_data->card[1] &&
			sd->status.cart[i].card[2] == item_data->card[2] && sd->status.cart[i].card[3] == item_data->card[3] );
	};

	if( i < MAX_CART )
	{// item already in cart, stack it
		if(sd->status.cart[i].amount+amount > MAX_AMOUNT)
			return 1; // no room

		sd->status.cart[i].amount+=amount;
		clif_cart_additem(sd,i,amount,0);
	}
	else
	{// item not stackable or not present, add it
		ARR_FIND( 0, MAX_CART, i, sd->status.cart[i].nameid == 0 );
		if( i == MAX_CART )
			return 1; // no room

		memcpy(&sd->status.cart[i],item_data,sizeof(sd->status.cart[0]));
		sd->status.cart[i].amount=amount;
		sd->cart_num++;
		clif_cart_additem(sd,i,amount,0);
	}

	sd->cart_weight += w;
	clif_updatestatus(sd,SP_CARTINFO);

	return 0;
}

/*==========================================
 * J?gACeð¸ç·
 *------------------------------------------*/
int pc_cart_delitem(struct map_session_data *sd,int n,int amount,int type)
{
	nullpo_retr(1, sd);

	if(sd->status.cart[n].nameid==0 ||
	   sd->status.cart[n].amount<amount)
		return 1;

	sd->status.cart[n].amount -= amount;
	sd->cart_weight -= itemdb_weight(sd->status.cart[n].nameid)*amount ;
	if(sd->status.cart[n].amount <= 0){
		memset(&sd->status.cart[n],0,sizeof(sd->status.cart[0]));
		sd->cart_num--;
	}
	if(!type) {
		clif_cart_delitem(sd,n,amount);
		clif_updatestatus(sd,SP_CARTINFO);
	}

	return 0;
}

/*==========================================
 * J?gÖACeÚ®
 *------------------------------------------*/
int pc_putitemtocart(struct map_session_data *sd,int idx,int amount)
{
	struct item *item_data;

	nullpo_retr(0, sd);

	if (idx < 0 || idx >= MAX_INVENTORY) //Invalid index check [Skotlex]
		return 1;
	
	item_data = &sd->status.inventory[idx];

	if( item_data->nameid == 0 || amount < 1 || item_data->amount < amount || sd->vender_id )
		return 1;

	if( pc_cart_additem(sd,item_data,amount) == 0 )
		return pc_delitem(sd,idx,amount,0);

	return 1;
}

/*==========================================
 * J?g?ÌACe?mF(Â?Ì·ªðÔ·)
 *------------------------------------------*/
int pc_cartitem_amount(struct map_session_data* sd, int idx, int amount)
{
	struct item* item_data;

	nullpo_retr(-1, sd);

	item_data = &sd->status.cart[idx];
	if( item_data->nameid == 0 || item_data->amount == 0 )
		return -1;

	return item_data->amount - amount;
}

/*==========================================
 * J?g©çACeÚ®
 *------------------------------------------*/
int pc_getitemfromcart(struct map_session_data *sd,int idx,int amount)
{
	struct item *item_data;
	int flag;

	nullpo_retr(0, sd);

	if (idx < 0 || idx >= MAX_CART) //Invalid index check [Skotlex]
		return 1;
	
	item_data=&sd->status.cart[idx];

	if(item_data->nameid==0 || amount < 1 || item_data->amount<amount || sd->vender_id )
		return 1;
	if((flag = pc_additem(sd,item_data,amount)) == 0)
		return pc_cart_delitem(sd,idx,amount,0);

	clif_additem(sd,0,0,flag);
	return 1;
}

/*==========================================
 * XeBiöJ
 *------------------------------------------*/
int pc_show_steal(struct block_list *bl,va_list ap)
{
	struct map_session_data *sd;
	int itemid;

	struct item_data *item=NULL;
	char output[100];

	sd=va_arg(ap,struct map_session_data *);
	itemid=va_arg(ap,int);

	if((item=itemdb_exists(itemid))==NULL)
		sprintf(output,"%s stole an Unknown Item (id: %i).",sd->status.name, itemid);
	else
		sprintf(output,"%s stole %s.",sd->status.name,item->jname);
	clif_displaymessage( ((struct map_session_data *)bl)->fd, output);

	return 0;
}
/*==========================================
 *
 *------------------------------------------*/
int pc_steal_item(struct map_session_data *sd,struct block_list *bl, int lv)
{
	int i,itemid,flag;
	double rate;
	struct status_data *sd_status, *md_status;
	struct mob_data *md;
	struct item tmp_item;

	if(!sd || !bl || bl->type!=BL_MOB)
		return 0;

	md = (TBL_MOB *)bl;

	if(md->state.steal_flag == UCHAR_MAX || md->sc.opt1) //already stolen from / status change check
		return 0;
	
	sd_status= status_get_status_data(&sd->bl);
	md_status= status_get_status_data(bl);

	if( md->master_id || md_status->mode&MD_BOSS ||
		(md->class_ >= 1324 && md->class_ < 1364) || // Treasure Boxes WoE
		(md->class_ >= 1938 && md->class_ < 1946) || // Treasure Boxes WoE SE
		map[bl->m].flag.nomobloot || // check noloot map flag [Lorky]
		(battle_config.skill_steal_max_tries && //Reached limit of steal attempts. [Lupus]
			md->state.steal_flag++ >= battle_config.skill_steal_max_tries)
  	) { //Can't steal from
		md->state.steal_flag = UCHAR_MAX;
		return 0;
	}

	// base skill success chance (percentual)
	rate = (sd_status->dex - md_status->dex)/2 + lv*6 + 4;
	rate += sd->add_steal_rate;
		
	if( rate < 1 )
		return 0;

	// Try dropping one item, in the order from first to last possible slot.
	// Droprate is affected by the skill success rate.
	for( i = 0; i < MAX_STEAL_DROP; i++ )
		if( md->db->dropitem[i].nameid > 0 && itemdb_exists(md->db->dropitem[i].nameid) && rand() % 10000 < md->db->dropitem[i].p * rate/100. )
			break;
	if( i == MAX_STEAL_DROP )
		return 0;

	itemid = md->db->dropitem[i].nameid;
	memset(&tmp_item,0,sizeof(tmp_item));
	tmp_item.nameid = itemid;
	tmp_item.amount = 1;
	tmp_item.identify = itemdb_isidentified(itemid);
	flag = pc_additem(sd,&tmp_item,1);

	//TODO: Should we disable stealing when the item you stole couldn't be added to your inventory? Perhaps players will figure out a way to exploit this behaviour otherwise?
	md->state.steal_flag = UCHAR_MAX; //you can't steal from this mob any more

	if(flag) { //Failed to steal due to overweight
		clif_additem(sd,0,0,flag);
		return 0;
	}
	
	if(battle_config.show_steal_in_same_party)
		party_foreachsamemap(pc_show_steal,sd,AREA_SIZE,sd,tmp_item.nameid);

	//Logs items, Stolen from mobs [Lupus]
	if(log_config.enable_logs&0x80) {
		log_pick_mob(md, "M", itemid, -1, NULL);
		log_pick_pc(sd, "P", itemid, 1, NULL);
	}
		
	//A Rare Steal Global Announce by Lupus
	if(md->db->dropitem[i].p<=battle_config.rare_drop_announce) {
		struct item_data *i_data;
		char message[128];
		i_data = itemdb_search(itemid);
		sprintf (message, msg_txt(542), (sd->status.name != NULL)?sd->status.name :"GM", md->db->jname, i_data->jname, (float)md->db->dropitem[i].p/100);
		//MSG: "'%s' stole %s's %s (chance: %0.02f%%)"
		intif_broadcast(message,strlen(message)+1,0);
	}
	return 1;
}

/*==========================================
 *
 *------------------------------------------*/
int pc_steal_coin(struct map_session_data *sd,struct block_list *target)
{
	int rate,skill;
	struct mob_data *md;
	if(!sd || !target || target->type != BL_MOB)
		return 0;

	md = (TBL_MOB*)target;
	if( md->state.steal_coin_flag || md->sc.data[SC_STONE] || md->sc.data[SC_FREEZE] )
		return 0;

	if( (md->class_ >= 1324 && md->class_ < 1364) || (md->class_ >= 1938 && md->class_ < 1946) )
		return 0;

	skill = pc_checkskill(sd,RG_STEALCOIN)*10;
	rate = skill + (sd->status.base_level - md->level)*3 + sd->battle_status.dex*2 + sd->battle_status.luk*2;
	if(rand()%1000 < rate) {
		pc_getzeny(sd,md->level*10 + rand()%100);
		md->state.steal_coin_flag = 1;
		return 1;
	}
	return 0;
}

/*==========================================
 * Set's a player position.
 * Return values:
 * 0 - Success.
 * 1 - Invalid map index.
 * 2 - Map not in this map-server, and failed to locate alternate map-server.
 *------------------------------------------*/
int pc_setpos(struct map_session_data* sd, unsigned short mapindex, int x, int y, uint8 clrtype)
{
	struct party_data *p;
	int m;

	nullpo_retr(0, sd);

	if( !mapindex || !mapindex_id2name(mapindex) )
	{
		ShowDebug("pc_setpos: Passed mapindex(%d) is invalid!\n", mapindex);
		return 1;
	}

	if( pc_isdead(sd) )
	{ //Revive dead people before warping them
		pc_setstand(sd);
		pc_setrestartvalue(sd,1);
	}

	m = map_mapindex2mapid(mapindex);
	if( map[m].flag.src4instance && sd->status.party_id && (p = party_search(sd->status.party_id)) != NULL && p->instance_id )
	{
		// Request the mapid of this src map into the instance of the party
		int im = instance_map2imap(m, p->instance_id);
		if( im < 0 )
			; // Player will enter the src map for instances
		else
		{ // Changes destiny to the instance map, not the source map
			m = im;
			mapindex = map_id2index(m);
		}
	}

	sd->state.changemap = (sd->mapindex != mapindex);
	if( sd->state.changemap )
	{ // Misc map-changing settings
		sd->state.pmap = sd->bl.m;
		if (sd->sc.count)
		{ // Cancel some map related stuff.
			if (sd->sc.data[SC_JAILED])
				return 1; //You may not get out!
			if (sd->sc.data[SC_BOSSMAPINFO])
				status_change_end(&sd->bl,SC_BOSSMAPINFO,-1);
			if (sd->sc.data[SC_WARM])
				status_change_end(&sd->bl,SC_WARM,-1);
			if (sd->sc.data[SC_SUN_COMFORT])
				status_change_end(&sd->bl,SC_SUN_COMFORT,-1);
			if (sd->sc.data[SC_MOON_COMFORT])
				status_change_end(&sd->bl,SC_MOON_COMFORT,-1);
			if (sd->sc.data[SC_STAR_COMFORT])
				status_change_end(&sd->bl,SC_STAR_COMFORT,-1);
			if (sd->sc.data[SC_MIRACLE])
				status_change_end(&sd->bl,SC_MIRACLE,-1);
			if (sd->sc.data[SC_KNOWLEDGE]) {
				struct status_change_entry *sce = sd->sc.data[SC_KNOWLEDGE];
				if (sce->timer != -1)
					delete_timer(sce->timer, status_change_timer);
				sce->timer = add_timer(gettick() + skill_get_time(SG_KNOWLEDGE, sce->val1), status_change_timer, sd->bl.id, SC_KNOWLEDGE);
			}
		}
		if (battle_config.clear_unit_onwarp&BL_PC)
			skill_clear_unitgroup(&sd->bl);
		party_send_dot_remove(sd); //minimap dot fix [Kevin]
		guild_send_dot_remove(sd);
		bg_send_dot_remove(sd);
		if (sd->regen.state.gc)
			sd->regen.state.gc = 0;
	}

	if( m < 0 )
	{
		uint32 ip;
		uint16 port;
		//if can't find any map-servers, just abort setting position.
		if(!sd->mapindex || map_mapname2ipport(mapindex,&ip,&port))
			return 2;

		if (sd->npc_id)
			npc_event_dequeue(sd);
		npc_script_event(sd, NPCE_LOGOUT);
		//remove from map, THEN change x/y coordinates
		unit_remove_map_pc(sd,clrtype);
		sd->mapindex = mapindex;
		sd->bl.x=x;
		sd->bl.y=y;
		pc_clean_skilltree(sd);
		chrif_save(sd,2);
		chrif_changemapserver(sd, ip, (short)port);

		//Free session data from this map server [Kevin]
		unit_free_pc(sd);

		return 0;
	}

	if( x < 0 || x >= map[m].xs || y < 0 || y >= map[m].ys )
	{
		ShowError("pc_setpos: attempt to place player %s (%d:%d) on invalid coordinates (%s-%d,%d)\n", sd->status.name, sd->status.account_id, sd->status.char_id, mapindex_id2name(mapindex),x,y);
		x = y = 0; // make it random
	}

	if( x == 0 && y == 0 )
	{// pick a random walkable cell
		do {
			x=rand()%(map[m].xs-2)+1;
			y=rand()%(map[m].ys-2)+1;
		} while(map_getcell(m,x,y,CELL_CHKNOPASS));
	}

	if(sd->bl.prev != NULL){
		unit_remove_map_pc(sd,clrtype);
		clif_changemap(sd,map[m].index,x,y); // [MouseJstr]
	} else if(sd->state.active)
		//Tag player for rewarping after map-loading is done. [Skotlex]
		sd->state.rewarp = 1;
	
	sd->mapindex = mapindex;
	sd->bl.m = m;
	sd->bl.x = sd->ud.to_x = x;
	sd->bl.y = sd->ud.to_y = y;

	if( sd->status.guild_id > 0 && map[m].flag.gvg_castle )
	{	// Increased guild castle regen [Valaris]
		struct guild_castle *gc = guild_mapindex2gc(sd->mapindex);
		if(gc && gc->guild_id == sd->status.guild_id)
			sd->regen.state.gc = 1;
	}

	if( sd->status.pet_id > 0 && sd->pd && sd->pd->pet.intimate > 0 )
	{
		sd->pd->bl.m = m;
		sd->pd->bl.x = sd->pd->ud.to_x = x;
		sd->pd->bl.y = sd->pd->ud.to_y = y;
		sd->pd->ud.dir = sd->ud.dir;
	}

	if( merc_is_hom_active(sd->hd) )
	{
		sd->hd->bl.m = m;
		sd->hd->bl.x = sd->hd->ud.to_x = x;
		sd->hd->bl.y = sd->hd->ud.to_y = y;
		sd->hd->ud.dir = sd->ud.dir;
	}

	if( sd->md )
	{
		sd->md->bl.m = m;
		sd->md->bl.x = sd->md->ud.to_x = x;
		sd->md->bl.y = sd->md->ud.to_y = y;
		sd->md->ud.dir = sd->ud.dir;
	}

	return 0;
}

/*==========================================
 * PCÌ_?v
 *------------------------------------------*/
int pc_randomwarp(struct map_session_data *sd, int type)
{
	int x,y,i=0;
	int m;

	nullpo_retr(0, sd);

	m=sd->bl.m;

	if (map[sd->bl.m].flag.noteleport)	// e|?gÖ~
		return 0;

	do{
		x=rand()%(map[m].xs-2)+1;
		y=rand()%(map[m].ys-2)+1;
	}while(map_getcell(m,x,y,CELL_CHKNOPASS) && (i++)<1000 );

	if (i < 1000)
		return pc_setpos(sd,map[sd->bl.m].index,x,y,type);

	return 0;
}

/*==========================================
 * Records a memo point at sd's current position
 * pos - entry to replace, (-1: shift oldest entry out)
 *------------------------------------------*/
int pc_memo(struct map_session_data* sd, int pos)
{
	int skill;

	nullpo_retr(0, sd);

	// check mapflags
	if( sd->bl.m >= 0 && (map[sd->bl.m].flag.nomemo || map[sd->bl.m].flag.nowarpto) && battle_config.any_warp_GM_min_level > pc_isGM(sd) ) {
		clif_skill_teleportmessage(sd, 1); // "Saved point cannot be memorized."
		return 0;
	}

	// check inputs
	if( pos < -1 || pos >= MAX_MEMOPOINTS )
		return 0; // invalid input

	// check required skill level
	skill = pc_checkskill(sd, AL_WARP);
	if( skill < 1 ) {
		clif_skill_memomessage(sd,2); // "You haven't learned Warp."
		return 0;
	}
	if( skill < 2 || skill - 2 < pos ) {
		clif_skill_memomessage(sd,1); // "Skill Level is not high enough."
		return 0;
	}

	if( pos == -1 )
	{
		int i;
		// prevent memo-ing the same map multiple times
		ARR_FIND( 0, MAX_MEMOPOINTS, i, sd->status.memo_point[i].map == map_id2index(sd->bl.m) );
		memmove(&sd->status.memo_point[1], &sd->status.memo_point[0], (min(i,MAX_MEMOPOINTS-1))*sizeof(struct point));
		pos = 0;
	}

	sd->status.memo_point[pos].map = map_id2index(sd->bl.m);
	sd->status.memo_point[pos].x = sd->bl.x;
	sd->status.memo_point[pos].y = sd->bl.y;

	clif_skill_memomessage(sd, 0);

	return 1;
}

//
// í??
//
/*==========================================
 * XLÌ?õ LµÄ¢½êLvªÔé
 *------------------------------------------*/
int pc_checkskill(struct map_session_data *sd,int skill_id)
{
	if(sd == NULL) return 0;
	if( skill_id>=GD_SKILLBASE){
		struct guild *g;
		if( sd->status.guild_id>0 && (g=guild_search(sd->status.guild_id))!=NULL)
			return guild_checkskill(g,skill_id);
		return 0;
	}

	if(sd->status.skill[skill_id].id == skill_id)
		return (sd->status.skill[skill_id].lv);

	return 0;
}

/*==========================================
 * í?XÉæéXLÌ??`FbN
 * ø?F
 *   struct map_session_data *sd	ZbVf?^
 *   int nameid						?õiID
 * ÔèlF
 *   0		?XÈµ
 *   -1		XLðð
 *------------------------------------------*/
int pc_checkallowskill(struct map_session_data *sd)
{
	const enum sc_type scw_list[] = {
		SC_TWOHANDQUICKEN,
		SC_ONEHAND,
		SC_AURABLADE,
		SC_PARRYING,
		SC_SPEARQUICKEN,
		SC_ADRENALINE,
		SC_ADRENALINE2,
		SC_GATLINGFEVER
	};
	const enum sc_type scs_list[] = {
		SC_AUTOGUARD,
		SC_DEFENDER,
		SC_REFLECTSHIELD
	};
	int i;
	nullpo_retr(0, sd);

	if(!sd->sc.count)
		return 0;
	
	for (i = 0; i < ARRAYLENGTH(scw_list); i++)
	{	// Skills requiring specific weapon types
		if(sd->sc.data[scw_list[i]] &&
			!pc_check_weapontype(sd,skill_get_weapontype(status_sc2skill(scw_list[i]))))
			status_change_end(&sd->bl,scw_list[i],-1);
	}
	
	if(sd->sc.data[SC_SPURT] && sd->status.weapon)
		// Spurt requires bare hands (feet, in fact xD)
		status_change_end(&sd->bl,SC_SPURT,-1);
	
	if(sd->status.shield <= 0) { // Skills requiring a shield
		for (i = 0; i < ARRAYLENGTH(scs_list); i++)
			if(sd->sc.data[scs_list[i]])
				status_change_end(&sd->bl,scs_list[i],-1);
	}
	return 0;
}

/*==========================================
 * ? õiÌ`FbN
 *------------------------------------------*/
int pc_checkequip(struct map_session_data *sd,int pos)
{
	int i;

	nullpo_retr(-1, sd);

	for(i=0;i<EQI_MAX;i++){
		if(pos & equip_pos[i])
			return sd->equip_index[i];
	}

	return -1;
}

/*==========================================
 * Convert's from the client's lame Job ID system
 * to the map server's 'makes sense' system. [Skotlex]
 *------------------------------------------*/
int pc_jobid2mapid(unsigned short b_class)
{
	int class_ = 0;
	if (b_class >= JOB_BABY && b_class <= JOB_SUPER_BABY)
	{
		if (b_class == JOB_SUPER_BABY)
			b_class = JOB_SUPER_NOVICE;
		else
			b_class -= JOB_BABY;
		class_|= JOBL_BABY;
	}
	else if (b_class >= JOB_NOVICE_HIGH && b_class <= JOB_PALADIN2)
	{
		b_class -= JOB_NOVICE_HIGH;
		class_|= JOBL_UPPER;
	}
	if (b_class >= JOB_KNIGHT && b_class <= JOB_KNIGHT2)
		class_|= JOBL_2_1;
	else if (b_class >= JOB_CRUSADER && b_class <= JOB_CRUSADER2)
		class_|= JOBL_2_2;
	switch (b_class)
	{
		case JOB_NOVICE:
		case JOB_SWORDMAN:
		case JOB_MAGE:
		case JOB_ARCHER:
		case JOB_ACOLYTE:
		case JOB_MERCHANT:
		case JOB_THIEF:
			class_ |= b_class;
			break;
		case JOB_KNIGHT:
		case JOB_KNIGHT2:
		case JOB_CRUSADER:
		case JOB_CRUSADER2:
			class_ |= MAPID_SWORDMAN;
			break;
		case JOB_PRIEST:
		case JOB_MONK:
			class_ |= MAPID_ACOLYTE;
			break;
		case JOB_WIZARD:
		case JOB_SAGE:
			class_ |= MAPID_MAGE;
			break;
		case JOB_BLACKSMITH:
		case JOB_ALCHEMIST:
			class_ |= MAPID_MERCHANT;
			break;
		case JOB_HUNTER:
		case JOB_BARD:
		case JOB_DANCER:
			class_ |= MAPID_ARCHER;
			break;
		case JOB_ASSASSIN:
		case JOB_ROGUE:
			class_ |= MAPID_THIEF;
			break;
			
		case JOB_STAR_GLADIATOR:
		case JOB_STAR_GLADIATOR2:
			class_ |= JOBL_2_1;
			class_ |= MAPID_TAEKWON;
			break;	
		case JOB_SOUL_LINKER:
			class_ |= JOBL_2_2;
		case JOB_TAEKWON:
			class_ |= MAPID_TAEKWON;
			break;
		case JOB_WEDDING:
			class_ = MAPID_WEDDING;
			break;
		case JOB_SUPER_NOVICE: //Super Novices are considered 2-1 novices. [Skotlex]
			class_ |= JOBL_2_1;
			break;
		case JOB_GUNSLINGER:
			class_ |= MAPID_GUNSLINGER;
			break;
		case JOB_NINJA:
			class_ |= MAPID_NINJA;
			break;
		case JOB_XMAS:
			class_ = MAPID_XMAS;
			break;
		case JOB_SUMMER:
			class_ = MAPID_SUMMER;
			break;
		default:
			return -1;
	}
	return class_;
}

//Reverts the map-style class id to the client-style one.
int pc_mapid2jobid(unsigned short class_, int sex)
{
	switch(class_) {
		case MAPID_NOVICE:          return JOB_NOVICE;
		case MAPID_SWORDMAN:        return JOB_SWORDMAN;
		case MAPID_MAGE:            return JOB_MAGE;
		case MAPID_ARCHER:          return JOB_ARCHER;
		case MAPID_ACOLYTE:         return JOB_ACOLYTE;
		case MAPID_MERCHANT:        return JOB_MERCHANT;
		case MAPID_THIEF:           return JOB_THIEF;
		case MAPID_TAEKWON:         return JOB_TAEKWON;
		case MAPID_WEDDING:         return JOB_WEDDING;
		case MAPID_GUNSLINGER:      return JOB_GUNSLINGER;
		case MAPID_NINJA:           return JOB_NINJA;
		case MAPID_XMAS:            return JOB_XMAS;
		case MAPID_SUMMER:          return JOB_SUMMER;
	//2_1 classes
		case MAPID_SUPER_NOVICE:    return JOB_SUPER_NOVICE;
		case MAPID_KNIGHT:          return JOB_KNIGHT;
		case MAPID_WIZARD:          return JOB_WIZARD;
		case MAPID_HUNTER:          return JOB_HUNTER;
		case MAPID_PRIEST:          return JOB_PRIEST;
		case MAPID_BLACKSMITH:      return JOB_BLACKSMITH;
		case MAPID_ASSASSIN:        return JOB_ASSASSIN;
		case MAPID_STAR_GLADIATOR:  return JOB_STAR_GLADIATOR;
	//2_2 classes
		case MAPID_CRUSADER:        return JOB_CRUSADER;
		case MAPID_SAGE:            return JOB_SAGE;
		case MAPID_BARDDANCER:      return sex?JOB_BARD:JOB_DANCER;
		case MAPID_MONK:            return JOB_MONK;
		case MAPID_ALCHEMIST:       return JOB_ALCHEMIST;
		case MAPID_ROGUE:           return JOB_ROGUE;
		case MAPID_SOUL_LINKER:     return JOB_SOUL_LINKER;
	//1-1: advanced
		case MAPID_NOVICE_HIGH:     return JOB_NOVICE_HIGH;
		case MAPID_SWORDMAN_HIGH:   return JOB_SWORDMAN_HIGH;
		case MAPID_MAGE_HIGH:       return JOB_MAGE_HIGH;
		case MAPID_ARCHER_HIGH:     return JOB_ARCHER_HIGH;
		case MAPID_ACOLYTE_HIGH:    return JOB_ACOLYTE_HIGH;
		case MAPID_MERCHANT_HIGH:   return JOB_MERCHANT_HIGH;
		case MAPID_THIEF_HIGH:      return JOB_THIEF_HIGH;
	//2_1 advanced
		case MAPID_LORD_KNIGHT:     return JOB_LORD_KNIGHT;
		case MAPID_HIGH_WIZARD:     return JOB_HIGH_WIZARD;
		case MAPID_SNIPER:          return JOB_SNIPER;
		case MAPID_HIGH_PRIEST:     return JOB_HIGH_PRIEST;
		case MAPID_WHITESMITH:      return JOB_WHITESMITH;
		case MAPID_ASSASSIN_CROSS:  return JOB_ASSASSIN_CROSS;
	//2_2 advanced
		case MAPID_PALADIN:         return JOB_PALADIN;
		case MAPID_PROFESSOR:       return JOB_PROFESSOR;
		case MAPID_CLOWNGYPSY:      return sex?JOB_CLOWN:JOB_GYPSY;
		case MAPID_CHAMPION:        return JOB_CHAMPION;
		case MAPID_CREATOR:         return JOB_CREATOR;
		case MAPID_STALKER:         return JOB_STALKER;
	//1-1 baby
		case MAPID_BABY:            return JOB_BABY;
		case MAPID_BABY_SWORDMAN:   return JOB_BABY_SWORDMAN;
		case MAPID_BABY_MAGE:       return JOB_BABY_MAGE;
		case MAPID_BABY_ARCHER:     return JOB_BABY_ARCHER;
		case MAPID_BABY_ACOLYTE:    return JOB_BABY_ACOLYTE;
		case MAPID_BABY_MERCHANT:   return JOB_BABY_MERCHANT;
		case MAPID_BABY_THIEF:      return JOB_BABY_THIEF;
	//2_1 baby
		case MAPID_SUPER_BABY:      return JOB_SUPER_BABY;
		case MAPID_BABY_KNIGHT:     return JOB_BABY_KNIGHT;
		case MAPID_BABY_WIZARD:     return JOB_BABY_WIZARD;
		case MAPID_BABY_HUNTER:     return JOB_BABY_HUNTER;
		case MAPID_BABY_PRIEST:     return JOB_BABY_PRIEST;
		case MAPID_BABY_BLACKSMITH: return JOB_BABY_BLACKSMITH;
		case MAPID_BABY_ASSASSIN:   return JOB_BABY_ASSASSIN;
	//2_2 baby
		case MAPID_BABY_CRUSADER:   return JOB_BABY_CRUSADER;
		case MAPID_BABY_SAGE:       return JOB_BABY_SAGE;
		case MAPID_BABY_BARDDANCER: return sex?JOB_BABY_BARD:JOB_BABY_DANCER;
		case MAPID_BABY_MONK:       return JOB_BABY_MONK;
		case MAPID_BABY_ALCHEMIST:  return JOB_BABY_ALCHEMIST;
		case MAPID_BABY_ROGUE:      return JOB_BABY_ROGUE;
		default:
			return -1;
	}
}

/*====================================================
 * This function return the name of the job (by [Yor])
 *----------------------------------------------------*/
char* job_name(int class_)
{
	switch (class_) {
	case JOB_NOVICE:
	case JOB_SWORDMAN:
	case JOB_MAGE:
	case JOB_ARCHER:
	case JOB_ACOLYTE:
	case JOB_MERCHANT:
	case JOB_THIEF:
		return msg_txt(550 - JOB_NOVICE+class_);
		
	case JOB_KNIGHT:
	case JOB_PRIEST:
	case JOB_WIZARD:
	case JOB_BLACKSMITH:
	case JOB_HUNTER:
	case JOB_ASSASSIN:
		return msg_txt(557 - JOB_KNIGHT+class_);
		
	case JOB_KNIGHT2:
		return msg_txt(557);
		
	case JOB_CRUSADER:
	case JOB_MONK:
	case JOB_SAGE:
	case JOB_ROGUE:
	case JOB_ALCHEMIST:
	case JOB_BARD:
	case JOB_DANCER:
		return msg_txt(563 - JOB_CRUSADER+class_);
			
	case JOB_CRUSADER2:
		return msg_txt(563);
		
	case JOB_WEDDING:
	case JOB_SUPER_NOVICE:

	case JOB_XMAS:
		return msg_txt(570 - JOB_WEDDING+class_);

	case JOB_SUMMER:
		return msg_txt(621);

	case JOB_NOVICE_HIGH:
	case JOB_SWORDMAN_HIGH:
	case JOB_MAGE_HIGH:
	case JOB_ARCHER_HIGH:
	case JOB_ACOLYTE_HIGH:
	case JOB_MERCHANT_HIGH:
	case JOB_THIEF_HIGH:
		return msg_txt(575 - JOB_NOVICE_HIGH+class_);

	case JOB_LORD_KNIGHT:
	case JOB_HIGH_PRIEST:
	case JOB_HIGH_WIZARD:
	case JOB_WHITESMITH:
	case JOB_SNIPER:
	case JOB_ASSASSIN_CROSS:
		return msg_txt(582 - JOB_LORD_KNIGHT+class_);
		
	case JOB_LORD_KNIGHT2:
		return msg_txt(582);
		
	case JOB_PALADIN:
	case JOB_CHAMPION:
	case JOB_PROFESSOR:
	case JOB_STALKER:
	case JOB_CREATOR:
	case JOB_CLOWN:
	case JOB_GYPSY:
		return msg_txt(588 - JOB_PALADIN + class_);
		
	case JOB_PALADIN2:
		return msg_txt(588);

	case JOB_BABY:
	case JOB_BABY_SWORDMAN:
	case JOB_BABY_MAGE:
	case JOB_BABY_ARCHER:
	case JOB_BABY_ACOLYTE:
	case JOB_BABY_MERCHANT:
	case JOB_BABY_THIEF:
		return msg_txt(595 - JOB_BABY + class_);
		
	case JOB_BABY_KNIGHT:
	case JOB_BABY_PRIEST:
	case JOB_BABY_WIZARD:
	case JOB_BABY_BLACKSMITH:
	case JOB_BABY_HUNTER:
	case JOB_BABY_ASSASSIN:
		return msg_txt(602 - JOB_BABY_KNIGHT + class_);
		
	case JOB_BABY_KNIGHT2:
		return msg_txt(602);
		
	case JOB_BABY_CRUSADER:
	case JOB_BABY_MONK:
	case JOB_BABY_SAGE:
	case JOB_BABY_ROGUE:
	case JOB_BABY_ALCHEMIST:
	case JOB_BABY_BARD:
	case JOB_BABY_DANCER:
		return msg_txt(608 - JOB_BABY_CRUSADER +class_);
		
	case JOB_BABY_CRUSADER2:
		return msg_txt(608);
		
	case JOB_SUPER_BABY:
		return msg_txt(615);
		
	case JOB_TAEKWON:
		return msg_txt(616);
	case JOB_STAR_GLADIATOR:
	case JOB_STAR_GLADIATOR2:
		return msg_txt(617);
	case JOB_SOUL_LINKER:
		return msg_txt(618);
		
	case JOB_GUNSLINGER:
		return msg_txt(619);
	case JOB_NINJA:
		return msg_txt(620);
	
	default:
		return msg_txt(650);
	}
}

int pc_follow_timer(int tid, unsigned int tick, int id, intptr data)
{
	struct map_session_data *sd;
	struct block_list *tbl;

	sd = map_id2sd(id);
	nullpo_retr(0, sd);

	if (sd->followtimer != tid){
		ShowError("pc_follow_timer %d != %d\n",sd->followtimer,tid);
		sd->followtimer = INVALID_TIMER;
		return 0;
	}

	sd->followtimer = INVALID_TIMER;
	if (pc_isdead(sd))
		return 0;

	if ((tbl = map_id2bl(sd->followtarget)) == NULL)
		return 0;

	if(status_isdead(tbl))
		return 0;

	// either player or target is currently detached from map blocks (could be teleporting),
	// but still connected to this map, so we'll just increment the timer and check back later
	if (sd->bl.prev != NULL && tbl->prev != NULL &&
		sd->ud.skilltimer == -1 && sd->ud.attacktimer == -1 && sd->ud.walktimer == -1)
	{
		if((sd->bl.m == tbl->m) && unit_can_reach_bl(&sd->bl,tbl, AREA_SIZE, 0, NULL, NULL)) {
			if (!check_distance_bl(&sd->bl, tbl, 5))
				unit_walktobl(&sd->bl, tbl, 5, 0);
		} else
			pc_setpos(sd, map_id2index(tbl->m), tbl->x, tbl->y, 3);
	}
	sd->followtimer = add_timer(
		tick + 1000,	// increase time a bit to loosen up map's load
		pc_follow_timer, sd->bl.id, 0);
	return 0;
}

int pc_stop_following (struct map_session_data *sd)
{
	nullpo_retr(0, sd);

	if (sd->followtimer != INVALID_TIMER) {
		delete_timer(sd->followtimer,pc_follow_timer);
		sd->followtimer = INVALID_TIMER;
	}
	sd->followtarget = -1;

	return 0;
}

int pc_follow(struct map_session_data *sd,int target_id)
{
	struct block_list *bl = map_id2bl(target_id);
	if (bl == NULL /*|| bl->type != BL_PC*/)
		return 1;
	if (sd->followtimer != -1)
		pc_stop_following(sd);

	sd->followtarget = target_id;
	pc_follow_timer(-1,gettick(),sd->bl.id,0);

	return 0;
}

int pc_checkbaselevelup(struct map_session_data *sd)
{
	unsigned int next = pc_nextbaseexp(sd);

	if (!next || sd->status.base_exp < next)
		return 0;
	do {
		sd->status.base_exp -= next;
		//Kyoki pointed out that the max overcarry exp is the exp needed for the previous level -1. [Skotlex]
		if(!battle_config.multi_level_up && sd->status.base_exp > next-1)
			sd->status.base_exp = next-1;

		sd->status.base_level ++;

		if (battle_config.use_statpoint_table)
			next = statp[sd->status.base_level] - statp[sd->status.base_level-1];
		else //Estimated way.
			next = (sd->status.base_level+14) / 5 ;

		sd->status.status_point += next;

	} while ((next=pc_nextbaseexp(sd)) > 0 && sd->status.base_exp >= next);

	if (battle_config.pet_lv_rate && sd->pd)	//<Skotlex> update pet's level
		status_calc_pet(sd->pd,0);
	
	clif_updatestatus(sd,SP_STATUSPOINT);
	clif_updatestatus(sd,SP_BASELEVEL);
	clif_updatestatus(sd,SP_NEXTBASEEXP);
	status_calc_pc(sd,0);
	status_percent_heal(&sd->bl,100,100);

	if((sd->class_&MAPID_UPPERMASK) == MAPID_SUPER_NOVICE)
	{
		sc_start(&sd->bl,status_skill2sc(PR_KYRIE),100,1,skill_get_time(PR_KYRIE,1));
		sc_start(&sd->bl,status_skill2sc(PR_IMPOSITIO),100,1,skill_get_time(PR_IMPOSITIO,1));
		sc_start(&sd->bl,status_skill2sc(PR_MAGNIFICAT),100,1,skill_get_time(PR_MAGNIFICAT,1));
		sc_start(&sd->bl,status_skill2sc(PR_GLORIA),100,1,skill_get_time(PR_GLORIA,1));
		sc_start(&sd->bl,status_skill2sc(PR_SUFFRAGIUM),100,1,skill_get_time(PR_SUFFRAGIUM,1));
		if (sd->state.snovice_dead_flag)
			sd->state.snovice_dead_flag = 0; //Reenable steelbody resurrection on dead.
	} else
	if((sd->class_&MAPID_UPPERMASK) == MAPID_TAEKWON || (sd->class_&MAPID_UPPERMASK) == MAPID_STAR_GLADIATOR)
	{
		sc_start(&sd->bl,status_skill2sc(AL_INCAGI),100,10,600000);
		sc_start(&sd->bl,status_skill2sc(AL_BLESSING),100,10,600000);
	}
	clif_misceffect(&sd->bl,0);
	npc_script_event(sd, NPCE_BASELVUP); //LORDALFA - LVLUPEVENT

	if(sd->status.party_id)
		party_send_levelup(sd);
	return 1;
}

int pc_checkjoblevelup(struct map_session_data *sd)
{
	unsigned int next = pc_nextjobexp(sd);

	nullpo_retr(0, sd);
	if(!next || sd->status.job_exp < next)
		return 0;

	do {
		sd->status.job_exp -= next;
		//Kyoki pointed out that the max overcarry exp is the exp needed for the previous level -1. [Skotlex]
		if(!battle_config.multi_level_up && sd->status.job_exp > next-1)
			sd->status.job_exp = next-1;

		sd->status.job_level ++;
		sd->status.skill_point ++;

	} while ((next=pc_nextjobexp(sd)) > 0 && sd->status.job_exp >= next);

	clif_updatestatus(sd,SP_JOBLEVEL);
	clif_updatestatus(sd,SP_NEXTJOBEXP);
	clif_updatestatus(sd,SP_SKILLPOINT);
	status_calc_pc(sd,0);
	clif_misceffect(&sd->bl,1);
	if (pc_checkskill(sd, SG_DEVIL) && !pc_nextjobexp(sd))
		clif_status_change(&sd->bl,SI_DEVIL, 1, 0); //Permanent blind effect from SG_DEVIL.

	npc_script_event(sd, NPCE_JOBLVUP);
	return 1;
}

/*==========================================
 * Alters experienced based on self bonuses that do not get even shared to the party.
 *------------------------------------------*/
static void pc_calcexp(struct map_session_data *sd, unsigned int *base_exp, unsigned int *job_exp, struct block_list *src)
{
	int bonus = 0;
	struct status_data *status = status_get_status_data(src);

	if (sd->expaddrace[status->race])
		bonus += sd->expaddrace[status->race];	
	bonus += sd->expaddrace[status->mode&MD_BOSS?RC_BOSS:RC_NONBOSS];

	if (battle_config.pk_mode && 
		(int)(status_get_lv(src) - sd->status.base_level) >= 20)
		bonus += 15; // pk_mode additional exp if monster >20 levels [Valaris]	

	if (sd->sc.data[SC_EXPBOOST])
		bonus += sd->sc.data[SC_EXPBOOST]->val1;

	*base_exp = (unsigned int) cap_value(*base_exp + (double)*base_exp * bonus/100., 1, UINT_MAX);

	if (sd->sc.data[SC_JEXPBOOST])
		bonus += sd->sc.data[SC_JEXPBOOST]->val1;

	*job_exp = (unsigned int) cap_value(*job_exp + (double)*job_exp * bonus/100., 1, UINT_MAX);

	return;
}
/*==========================================
 * ??læ¾
 *------------------------------------------*/
int pc_gainexp(struct map_session_data *sd, struct block_list *src, unsigned int base_exp,unsigned int job_exp,bool quest)
{
	float nextbp=0, nextjp=0;
	unsigned int nextb=0, nextj=0;
	nullpo_retr(0, sd);

	if(sd->bl.prev == NULL || pc_isdead(sd))
		return 0;

	if(!battle_config.pvp_exp && map[sd->bl.m].flag.pvp)  // [MouseJstr]
		return 0; // no exp on pvp maps

	if(sd->status.guild_id>0)
		base_exp-=guild_payexp(sd,base_exp);

	if(src) pc_calcexp(sd, &base_exp, &job_exp, src);

	nextb = pc_nextbaseexp(sd);
	nextj = pc_nextjobexp(sd);
		
	if(sd->state.showexp || battle_config.max_exp_gain_rate){
		if (nextb > 0)
			nextbp = (float) base_exp / (float) nextb;
		if (nextj > 0)
			nextjp = (float) job_exp / (float) nextj;

		if(battle_config.max_exp_gain_rate) {
			if (nextbp > battle_config.max_exp_gain_rate/1000.) {
				//Note that this value should never be greater than the original
				//base_exp, therefore no overflow checks are needed. [Skotlex]
				base_exp = (unsigned int)(battle_config.max_exp_gain_rate/1000.*nextb);
				if (sd->state.showexp)
					nextbp = (float) base_exp / (float) nextb;
			}
			if (nextjp > battle_config.max_exp_gain_rate/1000.) {
				job_exp = (unsigned int)(battle_config.max_exp_gain_rate/1000.*nextj);
				if (sd->state.showexp)
					nextjp = (float) job_exp / (float) nextj;
			}
		}
	}
	
	//Cap exp to the level up requirement of the previous level when you are at max level, otherwise cap at UINT_MAX (this is required for some S. Novice bonuses). [Skotlex]
	if (base_exp) {
		nextb = nextb?UINT_MAX:pc_thisbaseexp(sd);
		if(sd->status.base_exp > nextb - base_exp)
			sd->status.base_exp = nextb;
		else
			sd->status.base_exp += base_exp;
		pc_checkbaselevelup(sd);
		clif_updatestatus(sd,SP_BASEEXP);
	}

	if (job_exp) {
		nextj = nextj?UINT_MAX:pc_thisjobexp(sd);
		if(sd->status.job_exp > nextj - job_exp)
			sd->status.job_exp = nextj;
		else
			sd->status.job_exp += job_exp;
		pc_checkjoblevelup(sd);
		clif_updatestatus(sd,SP_JOBEXP);
	}

	if(sd->state.showexp){
#if PACKETVER >= 20091027
		if(base_exp)
			clif_displayexp(sd, base_exp, 1, quest);
		if(job_exp)
			clif_displayexp(sd, job_exp,  2, quest);
#else
		char output[256];
		sprintf(output,
			"Experience Gained Base:%u (%.2f%%) Job:%u (%.2f%%)",base_exp,nextbp*(float)100,job_exp,nextjp*(float)100);
		clif_disp_onlyself(sd,output,strlen(output));
#endif
	}

	return 1;
}

/*==========================================
 * Returns max level for this character.
 *------------------------------------------*/
unsigned int pc_maxbaselv(struct map_session_data *sd)
{
  	return max_level[pc_class2idx(sd->status.class_)][0];
};

unsigned int pc_maxjoblv(struct map_session_data *sd)
{
  	return max_level[pc_class2idx(sd->status.class_)][1];
};

/*==========================================
 * base level¤Kv??lvZ
 *------------------------------------------*/
unsigned int pc_nextbaseexp(struct map_session_data *sd)
{
	nullpo_retr(0, sd);

	if(sd->status.base_level>=pc_maxbaselv(sd) || sd->status.base_level<=0)
		return 0;

	return exp_table[pc_class2idx(sd->status.class_)][0][sd->status.base_level-1];
}

unsigned int pc_thisbaseexp(struct map_session_data *sd)
{
	if(sd->status.base_level>pc_maxbaselv(sd) || sd->status.base_level<=1)
		return 0;

	return exp_table[pc_class2idx(sd->status.class_)][0][sd->status.base_level-2];
}


/*==========================================
 * job level¤Kv??lvZ
 *------------------------------------------*/
unsigned int pc_nextjobexp(struct map_session_data *sd)
{
	nullpo_retr(0, sd);

	if(sd->status.job_level>=pc_maxjoblv(sd) || sd->status.job_level<=0)
		return 0;
	return exp_table[pc_class2idx(sd->status.class_)][1][sd->status.job_level-1];
}

unsigned int pc_thisjobexp(struct map_session_data *sd)
{
	if(sd->status.job_level>pc_maxjoblv(sd) || sd->status.job_level<=1)
		return 0;
	return exp_table[pc_class2idx(sd->status.class_)][1][sd->status.job_level-2];
}

/// Returns the value of the specified stat.
static int pc_getstat(struct map_session_data* sd, int type)
{
	nullpo_retr(-1, sd);

	switch( type ) {
	case SP_STR: return sd->status.str;
	case SP_AGI: return sd->status.agi;
	case SP_VIT: return sd->status.vit;
	case SP_INT: return sd->status.int_;
	case SP_DEX: return sd->status.dex;
	case SP_LUK: return sd->status.luk;
	default:
		return -1;
	}
}

/// Sets the specified stat to the specified value.
/// Returns the new value.
static int pc_setstat(struct map_session_data* sd, int type, int val)
{
	nullpo_retr(-1, sd);

	switch( type ) {
	case SP_STR: sd->status.str = val; break;
	case SP_AGI: sd->status.agi = val; break;
	case SP_VIT: sd->status.vit = val; break;
	case SP_INT: sd->status.int_ = val; break;
	case SP_DEX: sd->status.dex = val; break;
	case SP_LUK: sd->status.luk = val; break;
	default:
		return -1;
	}

	return val;
}

/// Returns the number of stat points needed to raise the specified stat by 1.
int pc_need_status_point(struct map_session_data* sd, int type)
{
	return ( 1 + (pc_getstat(sd,type) + 9) / 10 );
}

/// Raises a stat by 1.
/// Obeys max_parameter limits.
/// Subtracts stat points.
///
/// @param type The stat to change (see enum _sp)
int pc_statusup(struct map_session_data* sd, int type)
{
	int max, need, val;

	nullpo_retr(0, sd);

	// check conditions
	need = pc_need_status_point(sd,type);
	if( type < SP_STR || type > SP_LUK || need < 0 || need > sd->status.status_point )
	{
		clif_statusupack(sd,type,0,0);
		return 1;
	}

	// check limits
	max = pc_maxparameter(sd);
	if( pc_getstat(sd,type) >= max )
	{
		clif_statusupack(sd,type,0,0);
		return 1;
	}

	// set new values
	val = pc_setstat(sd, type, pc_getstat(sd,type) + 1);
	sd->status.status_point -= need;

	status_calc_pc(sd,0);

	// update increase cost indicator
	if( need != pc_need_status_point(sd,type) )
		clif_updatestatus(sd, SP_USTR + type-SP_STR);

	// update statpoint count
	clif_updatestatus(sd,SP_STATUSPOINT);

	// update stat value
	clif_statusupack(sd,type,1,val); // required
	if( val > 255 )
		clif_updatestatus(sd,type); // send after the 'ack' to override the truncated value

	return 0;
}

/// Raises a stat by the specified amount.
/// Obeys max_parameter limits.
/// Does not subtract stat points.
///
/// @param type The stat to change (see enum _sp)
/// @param val The stat increase amount.
int pc_statusup2(struct map_session_data* sd, int type, int val)
{
	int max, need;
	nullpo_retr(0, sd);

	if( type < SP_STR || type > SP_LUK )
	{
		clif_statusupack(sd,type,0,0);
		return 1;
	}

	need = pc_need_status_point(sd,type);

	// set new value
	max = pc_maxparameter(sd);
	val = pc_setstat(sd, type, cap_value(pc_getstat(sd,type) + val, 1, max));
	
	status_calc_pc(sd,0);

	// update increase cost indicator
	if( need != pc_need_status_point(sd,type) )
		clif_updatestatus(sd, SP_USTR + type-SP_STR);

	// update stat value
	clif_statusupack(sd,type,1,val); // required
	if( val > 255 )
		clif_updatestatus(sd,type); // send after the 'ack' to override the truncated value

	return 0;
}

/*==========================================
 * XL|CgèUè
 *------------------------------------------*/
int pc_skillup(struct map_session_data *sd,int skill_num)
{
	nullpo_retr(0, sd);

	if( skill_num >= GD_SKILLBASE && skill_num < GD_SKILLBASE+MAX_GUILDSKILL )
	{
		guild_skillup(sd, skill_num);
		return 0;
	}

	if( skill_num >= HM_SKILLBASE && skill_num < HM_SKILLBASE+MAX_HOMUNSKILL && sd->hd )
	{
		merc_hom_skillup(sd->hd, skill_num);
		return 0;
	}

	if( skill_num < 0 || skill_num >= MAX_SKILL )
		return 0;

	if( sd->status.skill_point > 0 &&
		sd->status.skill[skill_num].id &&
		sd->status.skill[skill_num].flag == 0 && //Don't allow raising while you have granted skills. [Skotlex]
		sd->status.skill[skill_num].lv < skill_tree_get_max(skill_num, sd->status.class_) )
	{
		sd->status.skill[skill_num].lv++;
		sd->status.skill_point--;
		if( !skill_get_inf(skill_num) ) 
			status_calc_pc(sd,0); // Only recalculate for passive skills.
		else if( sd->status.skill_point == 0 && (sd->class_&MAPID_UPPERMASK) == MAPID_TAEKWON && sd->status.base_level >= 90 && pc_famerank(sd->status.char_id, MAPID_TAEKWON) )
			pc_calc_skilltree(sd); // Required to grant all TK Ranger skills.
		else
			pc_check_skilltree(sd, skill_num); // Check if a new skill can Lvlup

		clif_skillup(sd,skill_num);
		clif_updatestatus(sd,SP_SKILLPOINT);
		clif_skillinfoblock(sd);
	}

	return 0;
}

/*==========================================
 * /allskill
 *------------------------------------------*/
int pc_allskillup(struct map_session_data *sd)
{
	int i,id;

	nullpo_retr(0, sd);

	for(i=0;i<MAX_SKILL;i++){
		if (sd->status.skill[i].flag && sd->status.skill[i].flag != 13){
			sd->status.skill[i].lv=(sd->status.skill[i].flag==1)?0:sd->status.skill[i].flag-2;
			sd->status.skill[i].flag=0;
			if (!sd->status.skill[i].lv)
				sd->status.skill[i].id=0;
		}
	}

	//pc_calc_skilltree takes care of setting the ID to valid skills. [Skotlex]
	if (battle_config.gm_allskill > 0 && pc_isGM(sd) >= battle_config.gm_allskill)
	{	//Get ALL skills except npc/guild ones. [Skotlex]
		//and except SG_DEVIL [Komurka] and MO_TRIPLEATTACK and RG_SNATCHER [ultramage]
		for(i=0;i<MAX_SKILL;i++){
			if(!(skill_get_inf2(i)&(INF2_NPC_SKILL|INF2_GUILD_SKILL)) && i!=SG_DEVIL && i!=MO_TRIPLEATTACK && i!=RG_SNATCHER)
				sd->status.skill[i].lv=skill_get_max(i); //Nonexistant skills should return a max of 0 anyway.
		}
	}
	else
	{
		int inf2;
		for(i=0;i < MAX_SKILL_TREE && (id=skill_tree[pc_class2idx(sd->status.class_)][i].id)>0;i++){
			inf2 = skill_get_inf2(id);
			if (
				(inf2&INF2_QUEST_SKILL && !battle_config.quest_skill_learn) ||
				(inf2&(INF2_WEDDING_SKILL|INF2_SPIRIT_SKILL)) ||
				id==SG_DEVIL
			)
				continue; //Cannot be learned normally.
			sd->status.skill[id].lv = skill_tree_get_max(id, sd->status.class_);	// celest
		}
	}
	status_calc_pc(sd,0);
	//Required because if you could level up all skills previously, 
	//the update will not be sent as only the lv variable changes.
	clif_skillinfoblock(sd);
	return 0;
}

/*==========================================
 * /resetlvl
 *------------------------------------------*/
int pc_resetlvl(struct map_session_data* sd,int type)
{
	int  i;

	nullpo_retr(0, sd);

	if (type != 3) //Also reset skills
		pc_resetskill(sd, 0);

	if(type == 1){
		sd->status.skill_point=0;
		sd->status.base_level=1;
		sd->status.job_level=1;
		sd->status.base_exp=sd->status.base_exp=0;
		sd->status.job_exp=sd->status.job_exp=0;
		if(sd->sc.option !=0)
			sd->sc.option = 0;

		sd->status.str=1;
		sd->status.agi=1;
		sd->status.vit=1;
		sd->status.int_=1;
		sd->status.dex=1;
		sd->status.luk=1;
		if(sd->status.class_ == JOB_NOVICE_HIGH) {
			sd->status.status_point=100;	// not 88 [celest]
			// give platinum skills upon changing
			pc_skill(sd,142,1,0);
			pc_skill(sd,143,1,0);
		}
	}

	if(type == 2){
		sd->status.skill_point=0;
		sd->status.base_level=1;
		sd->status.job_level=1;
		sd->status.base_exp=0;
		sd->status.job_exp=0;
	}
	if(type == 3){
		sd->status.base_level=1;
		sd->status.base_exp=0;
	}
	if(type == 4){
		sd->status.job_level=1;
		sd->status.job_exp=0;
	}

	clif_updatestatus(sd,SP_STATUSPOINT);
	clif_updatestatus(sd,SP_STR);
	clif_updatestatus(sd,SP_AGI);
	clif_updatestatus(sd,SP_VIT);
	clif_updatestatus(sd,SP_INT);
	clif_updatestatus(sd,SP_DEX);
	clif_updatestatus(sd,SP_LUK);
	clif_updatestatus(sd,SP_BASELEVEL);
	clif_updatestatus(sd,SP_JOBLEVEL);
	clif_updatestatus(sd,SP_STATUSPOINT);
	clif_updatestatus(sd,SP_NEXTBASEEXP);
	clif_updatestatus(sd,SP_NEXTJOBEXP);
	clif_updatestatus(sd,SP_SKILLPOINT);

	clif_updatestatus(sd,SP_USTR);	// Updates needed stat points - Valaris
	clif_updatestatus(sd,SP_UAGI);
	clif_updatestatus(sd,SP_UVIT);
	clif_updatestatus(sd,SP_UINT);
	clif_updatestatus(sd,SP_UDEX);
	clif_updatestatus(sd,SP_ULUK);	// End Addition

	for(i=0;i<EQI_MAX;i++) { // unequip items that can't be equipped by base 1 [Valaris]
		if(sd->equip_index[i] >= 0)
			if(!pc_isequip(sd,sd->equip_index[i]))
				pc_unequipitem(sd,sd->equip_index[i],2);
	}

	if ((type == 1 || type == 2 || type == 3) && sd->status.party_id)
		party_send_levelup(sd);

	status_calc_pc(sd,0);
	clif_skillinfoblock(sd);

	return 0;
}
/*==========================================
 * /resetstate
 *------------------------------------------*/
int pc_resetstate(struct map_session_data* sd)
{
	nullpo_retr(0, sd);
	
	if (battle_config.use_statpoint_table)
	{	// New statpoint table used here - Dexity
		if (sd->status.base_level > MAX_LEVEL)
		{	//statp[] goes out of bounds, can't reset!
			ShowError("pc_resetstate: Can't reset stats of %d:%d, the base level (%d) is greater than the max level supported (%d)\n",
				sd->status.account_id, sd->status.char_id, sd->status.base_level, MAX_LEVEL);
			return 0;
		}
		
		sd->status.status_point = statp[sd->status.base_level] + ( sd->class_&JOBL_UPPER ? 52 : 0 ); // extra 52+48=100 stat points
	}
	else
	{ //Use new stat-calculating equation [Skotlex]
#define sumsp(a) (((a-1)/10 +2)*(5*((a-1)/10 +1) + (a-1)%10) -10)
		int add=0;
		add += sumsp(sd->status.str);
		add += sumsp(sd->status.agi);
		add += sumsp(sd->status.vit);
		add += sumsp(sd->status.int_);
		add += sumsp(sd->status.dex);
		add += sumsp(sd->status.luk);

		sd->status.status_point+=add;
	}

	sd->status.str=1;
	sd->status.agi=1;
	sd->status.vit=1;
	sd->status.int_=1;
	sd->status.dex=1;
	sd->status.luk=1;

	clif_updatestatus(sd,SP_STR);
	clif_updatestatus(sd,SP_AGI);
	clif_updatestatus(sd,SP_VIT);
	clif_updatestatus(sd,SP_INT);
	clif_updatestatus(sd,SP_DEX);
	clif_updatestatus(sd,SP_LUK);

	clif_updatestatus(sd,SP_USTR);	// Updates needed stat points - Valaris
	clif_updatestatus(sd,SP_UAGI);
	clif_updatestatus(sd,SP_UVIT);
	clif_updatestatus(sd,SP_UINT);
	clif_updatestatus(sd,SP_UDEX);
	clif_updatestatus(sd,SP_ULUK);	// End Addition
	
	clif_updatestatus(sd,SP_STATUSPOINT);
	status_calc_pc(sd,0);

	return 1;
}

/*==========================================
 * /resetskill
 * if flag&1, perform block resync and status_calc call.
 * if flag&2, just count total amount of skill points used by player, do not really reset.
 *------------------------------------------*/
int pc_resetskill(struct map_session_data* sd, int flag)
{
	int i, lv, inf2, skill_point=0;
	nullpo_retr(0, sd);

	if( !(flag&2) )
	{ //Remove stuff lost when resetting skills.
		if( pc_checkskill(sd, SG_DEVIL) &&  !pc_nextjobexp(sd) )
			clif_status_load(&sd->bl, SI_DEVIL, 0); //Remove perma blindness due to skill-reset. [Skotlex]
		i = sd->sc.option;
		if( i&OPTION_RIDING && pc_checkskill(sd, KN_RIDING) )
			i &= ~OPTION_RIDING;
		if( i&OPTION_CART && pc_checkskill(sd, MC_PUSHCART) )
			i &= ~OPTION_CART;
		if( i&OPTION_FALCON && pc_checkskill(sd, HT_FALCON) )
			i &= ~OPTION_FALCON;

		if( i != sd->sc.option )
			pc_setoption(sd, i);

		if( merc_is_hom_active(sd->hd) && pc_checkskill(sd, AM_CALLHOMUN) )
			merc_hom_vaporize(sd, 0);
	}

	for( i = 1; i < MAX_SKILL; i++ )
	{
		lv = sd->status.skill[i].lv;
		if (lv < 1) continue;

		inf2 = skill_get_inf2(i);

		if( inf2&(INF2_WEDDING_SKILL|INF2_SPIRIT_SKILL) ) //Avoid reseting wedding/linker skills.
			continue;
		
		// Don't reset trick dead if not a novice/baby
		if( i == NV_TRICKDEAD && (sd->class_&MAPID_UPPERMASK) != MAPID_NOVICE && (sd->class_&MAPID_UPPERMASK) != MAPID_BABY )
		{
			sd->status.skill[i].lv = 0;
			sd->status.skill[i].flag = 0;
			continue;
		}

		if( i == NV_BASIC && (sd->class_&MAPID_UPPERMASK) != MAPID_NOVICE && (sd->class_&MAPID_UPPERMASK) != MAPID_BABY )
		{ // Official server does not include Basic Skill to be resetted. [Jobbie]
			sd->status.skill[i].lv = 9;
			sd->status.skill[i].flag = 0;
			continue;
		}

		if( inf2&INF2_QUEST_SKILL && !battle_config.quest_skill_learn )
		{ //Only handle quest skills in a special way when you can't learn them manually
			if( battle_config.quest_skill_reset && !(flag&2) )
			{	//Wipe them
				sd->status.skill[i].lv = 0;
				sd->status.skill[i].flag = 0;
			}
			continue;
		}
		if( !sd->status.skill[i].flag )
			skill_point += lv;
		else if( sd->status.skill[i].flag > 2 && sd->status.skill[i].flag != 13 )
			skill_point += (sd->status.skill[i].flag - 2);

		if( !(flag&2) )
		{
			sd->status.skill[i].lv = 0;
			sd->status.skill[i].flag = 0;
		}
	}
	
	if( flag&2 || !skill_point ) return skill_point;

	sd->status.skill_point += skill_point;

	if( flag&1 )
	{
		clif_updatestatus(sd,SP_SKILLPOINT);
		clif_skillinfoblock(sd);
		status_calc_pc(sd,0);
	}

	return skill_point;
}

/*==========================================
 * /resetfeel [Komurka]
 *------------------------------------------*/
int pc_resetfeel(struct map_session_data* sd)
{
	int i;
	nullpo_retr(0, sd);

	for (i=0; i<3; i++)
	{
		sd->feel_map[i].m = -1;
		sd->feel_map[i].index = 0;
		pc_setglobalreg(sd,sg_info[i].feel_var,0);
	}

	return 0;
}

int pc_resethate(struct map_session_data* sd)
{
	int i;
	nullpo_retr(0, sd);

	for (i=0; i<3; i++)
	{
		sd->hate_mob[i] = -1;
		pc_setglobalreg(sd,sg_info[i].hate_var,0);
	}
	return 0;
}

int pc_skillatk_bonus(struct map_session_data *sd, int skill_num)
{
	int i, bonus = 0;

	ARR_FIND(0, ARRAYLENGTH(sd->skillatk), i, sd->skillatk[i].id == skill_num);
	if( i < ARRAYLENGTH(sd->skillatk) ) bonus = sd->skillatk[i].val;

	return bonus;
}

int pc_skillheal_bonus(struct map_session_data *sd, int skill_num)
{
	int i, bonus = sd->add_heal_rate;

	if( bonus )
	{
		switch( skill_num )
		{
		case AL_HEAL:			if( !(battle_config.skill_add_heal_rate&1) ) bonus = 0; break;
		case PR_SANCTUARY:		if( !(battle_config.skill_add_heal_rate&2) ) bonus = 0; break;
		case AM_POTIONPITCHER:	if( !(battle_config.skill_add_heal_rate&4) ) bonus = 0; break;
		case CR_SLIMPITCHER:	if( !(battle_config.skill_add_heal_rate&8) ) bonus = 0; break;
		case BA_APPLEIDUN:		if( !(battle_config.skill_add_heal_rate&16) ) bonus = 0; break;
		}
	}

	ARR_FIND(0, ARRAYLENGTH(sd->skillheal), i, sd->skillheal[i].id == skill_num);
	if( i < ARRAYLENGTH(sd->skillheal) ) bonus += sd->skillheal[i].val;

	return bonus;
}

int pc_skillheal2_bonus(struct map_session_data *sd, int skill_num)
{
	int i, bonus = sd->add_heal2_rate;

	ARR_FIND(0, ARRAYLENGTH(sd->skillheal2), i, sd->skillheal2[i].id == skill_num);
	if( i < ARRAYLENGTH(sd->skillheal2) ) bonus += sd->skillheal2[i].val;

	return bonus;
}

void pc_respawn(struct map_session_data* sd, uint8 clrtype)
{
	if( !pc_isdead(sd) )
		return; // not applicable
	if( sd->state.bg_id && bg_member_respawn(sd) )
		return; // member revived by battleground

	pc_setstand(sd);
	pc_setrestartvalue(sd,3);
	if( pc_setpos(sd, sd->status.save_point.map, sd->status.save_point.x, sd->status.save_point.y, clrtype) )
		clif_resurrection(&sd->bl, 1); //If warping fails, send a normal stand up packet.
}

static int pc_respawn_timer(int tid, unsigned int tick, int id, intptr data)
{
	struct map_session_data *sd = map_id2sd(id);
	if( sd != NULL )
	{
		sd->pvp_point=0;
		pc_respawn(sd,0);
	}

	return 0;
}

/*==========================================
 * Invoked when a player has received damage
 *------------------------------------------*/
void pc_damage(struct map_session_data *sd,struct block_list *src,unsigned int hp, unsigned int sp)
{
	if (sp) clif_updatestatus(sd,SP_SP);
	if (hp) clif_updatestatus(sd,SP_HP);
	else return;
	
	if( !src || src == &sd->bl )
		return;

	if( pc_issit(sd) )
	{
		pc_setstand(sd);
		skill_sit(sd,0);
	}

	if( sd->progressbar.npc_id )
		clif_progressbar_abort(sd);

	if( sd->status.pet_id > 0 && sd->pd && battle_config.pet_damage_support )
		pet_target_check(sd,src,1);

	sd->canlog_tick = gettick();
}

int pc_dead(struct map_session_data *sd,struct block_list *src)
{
	int i=0,j=0,k=0;
	unsigned int tick = gettick();
		
	for(k = 0; k < 5; k++)
	if (sd->devotion[k]){
		struct map_session_data *devsd = map_id2sd(sd->devotion[k]);
		if (devsd) status_change_end(&devsd->bl,SC_DEVOTION,-1);
		sd->devotion[k] = 0;
	}

	if(sd->status.pet_id > 0 && sd->pd)
	{
		struct pet_data *pd = sd->pd;
		if( !map[sd->bl.m].flag.noexppenalty )
		{
			pet_set_intimate(pd, pd->pet.intimate - pd->petDB->die);
			if( pd->pet.intimate < 0 )
				pd->pet.intimate = 0;
			clif_send_petdata(sd,sd->pd,1,pd->pet.intimate);
		}
		if( sd->pd->target_id ) // Unlock all targets...
			pet_unlocktarget(sd->pd);
	}

	if( sd->status.hom_id > 0 && battle_config.homunculus_auto_vapor )
		merc_hom_vaporize(sd, 0);

	if( sd->md )
		merc_delete(sd->md, 3); // Your mercenary soldier has ran away.

	// Leave duel if you die [LuzZza]
	if(battle_config.duel_autoleave_when_die) {
		if(sd->duel_group > 0)
			duel_leave(sd->duel_group, sd);
		if(sd->duel_invite > 0)
			duel_reject(sd->duel_invite, sd);
	}

	pc_setglobalreg(sd,"PC_DIE_COUNTER",sd->die_counter+1);
	pc_setglobalreg(sd,"killerrid",src?src->id:0);
	if( sd->state.bg_id )
	{
		struct battleground_data *bg;
		if( (bg = bg_team_search(sd->state.bg_id)) != NULL && bg->die_event[0] )
			npc_event(sd, bg->die_event, 0);
	}
	npc_script_event(sd,NPCE_DIE);

	pc_setdead(sd);
	//Reset menu skills/item skills
	if (sd->skillitem)
		sd->skillitem = sd->skillitemlv = 0;
	if (sd->menuskill_id)
		sd->menuskill_id = sd->menuskill_val = 0;
	//Reset ticks.
	sd->hp_loss.tick = sd->sp_loss.tick = sd->hp_regen.tick = sd->sp_regen.tick = 0;

	if ( sd && sd->spiritball )
		pc_delspiritball(sd,sd->spiritball,0);

	if (src)
	switch (src->type) {
	case BL_MOB:
	{
		struct mob_data *md=(struct mob_data *)src;
		if(md->target_id==sd->bl.id)
			mob_unlocktarget(md,tick);
		if(battle_config.mobs_level_up && md->status.hp &&
			(unsigned int)md->level < pc_maxbaselv(sd) &&
			!md->guardian_data && !md->special_state.ai// Guardians/summons should not level. [Skotlex]
		) { 	// monster level up [Valaris]
			clif_misceffect(&md->bl,0);
			md->level++;
			status_calc_mob(md, 0);
			status_percent_heal(src,10,0);
		}
		src = battle_get_master(src); // Maybe Player Summon
	}
	break;
	case BL_PET: //Pass on to master...
		src = &((TBL_PET*)src)->msd->bl;
	break;
	case BL_HOM:
		src = &((TBL_HOM*)src)->master->bl;
	break;
	case BL_MER:
		src = &((TBL_MER*)src)->master->bl;
	break;
	}

	if (src && src->type == BL_PC)
	{
		struct map_session_data *ssd = (struct map_session_data *)src;
		pc_setglobalreg(ssd, "killedrid", sd->bl.id);
		npc_script_event(ssd, NPCE_KILLPC);

		if (battle_config.pk_mode&2) {
			ssd->status.manner -= 5;
			if(ssd->status.manner < 0)
				sc_start(src,SC_NOCHAT,100,0,0);
#if 0
			// PK/Karma system code (not enabled yet) [celest]
			// originally from Kade Online, so i don't know if any of these is correct ^^;
			// note: karma is measured REVERSE, so more karma = more 'evil' / less honourable,
			// karma going down = more 'good' / more honourable.
			// The Karma System way...
		
			if (sd->status.karma > ssd->status.karma) {	// If player killed was more evil
				sd->status.karma--;
				ssd->status.karma--;
			}
			else if (sd->status.karma < ssd->status.karma)	// If player killed was more good
				ssd->status.karma++;
	

			// or the PK System way...
	
			if (sd->status.karma > 0)	// player killed is dishonourable?
				ssd->status.karma--; // honour points earned
			sd->status.karma++;	// honour points lost
		
			// To-do: Receive exp on certain occasions
#endif
		}
	}

	if(battle_config.bone_drop==2
		|| (battle_config.bone_drop==1 && map[sd->bl.m].flag.pvp))
	{
		struct item item_tmp;
		memset(&item_tmp,0,sizeof(item_tmp));
		item_tmp.nameid=ITEMID_SKULL_;
		item_tmp.identify=1;
		item_tmp.card[0]=CARD0_CREATE;
		item_tmp.card[1]=0;
		item_tmp.card[2]=GetWord(sd->status.char_id,0); // CharId
		item_tmp.card[3]=GetWord(sd->status.char_id,1);
		map_addflooritem(&item_tmp,1,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0);
	}

	// activate Steel body if a super novice dies at 99+% exp [celest]
	if ((sd->class_&MAPID_UPPERMASK) == MAPID_SUPER_NOVICE && !sd->state.snovice_dead_flag)
  	{
		unsigned int next = pc_nextbaseexp(sd);
		if( next == 0 ) next = pc_thisbaseexp(sd);
		if( get_percentage(sd->status.base_exp,next) >= 99 && !map_flag_gvg(sd->bl.m) )
		{
			sd->state.snovice_dead_flag = 1;
			pc_setstand(sd);
			status_percent_heal(&sd->bl, 100, 100);
			clif_resurrection(&sd->bl, 1);
			if(battle_config.pc_invincible_time)
				pc_setinvincibletimer(sd, battle_config.pc_invincible_time);
			sc_start(&sd->bl,status_skill2sc(MO_STEELBODY),100,1,skill_get_time(MO_STEELBODY,1));
			if(map_flag_gvg(sd->bl.m))
				pc_respawn_timer(-1, gettick(), sd->bl.id, 0);
			return 0;
		}
	}

	// changed penalty options, added death by player if pk_mode [Valaris]
	if(battle_config.death_penalty_type
		&& (sd->class_&MAPID_UPPERMASK) != MAPID_NOVICE	// only novices will receive no penalty
		&& !map[sd->bl.m].flag.noexppenalty && !map_flag_gvg(sd->bl.m)
		&& !sd->sc.data[SC_BABY] && !sd->sc.data[SC_LIFEINSURANCE])
	{
		unsigned int base_penalty =0;
		if (battle_config.death_penalty_base > 0) {
			switch (battle_config.death_penalty_type) {
				case 1:
					base_penalty = (unsigned int) ((double)pc_nextbaseexp(sd) * (double)battle_config.death_penalty_base/10000);
				break;
				case 2:
					base_penalty = (unsigned int) ((double)sd->status.base_exp * (double)battle_config.death_penalty_base/10000);
				break;
			}
			if(base_penalty) {
			  	if (battle_config.pk_mode && src && src->type==BL_PC)
					base_penalty*=2;
				sd->status.base_exp -= min(sd->status.base_exp, base_penalty);
				clif_updatestatus(sd,SP_BASEEXP);
			}
		}
		if(battle_config.death_penalty_job > 0)
	  	{
			base_penalty = 0;
			switch (battle_config.death_penalty_type) {
				case 1:
					base_penalty = (unsigned int) ((double)pc_nextjobexp(sd) * (double)battle_config.death_penalty_job/10000);
				break;
				case 2:
					base_penalty = (unsigned int) ((double)sd->status.job_exp * (double)battle_config.death_penalty_job/10000);
				break;
			}
			if(base_penalty) {
			  	if (battle_config.pk_mode && src && src->type==BL_PC)
					base_penalty*=2;
				sd->status.job_exp -= min(sd->status.job_exp, base_penalty);
				clif_updatestatus(sd,SP_JOBEXP);
			}
		}
		if(battle_config.zeny_penalty > 0 && !map[sd->bl.m].flag.nozenypenalty)
	  	{
			base_penalty = (unsigned int)((double)sd->status.zeny * (double)battle_config.zeny_penalty / 10000.);
			if(base_penalty)
				pc_payzeny(sd, base_penalty);
		}
	}

	if(map[sd->bl.m].flag.pvp_nightmaredrop)
	{ // Moved this outside so it works when PVP isn't enabled and during pk mode [Ancyker]
		for(j=0;j<MAX_DROP_PER_MAP;j++){
			int id = map[sd->bl.m].drop_list[j].drop_id;
			int type = map[sd->bl.m].drop_list[j].drop_type;
			int per = map[sd->bl.m].drop_list[j].drop_per;
			if(id == 0)
				continue;
			if(id == -1){
				int eq_num=0,eq_n[MAX_INVENTORY];
				memset(eq_n,0,sizeof(eq_n));
				for(i=0;i<MAX_INVENTORY;i++){
					int k;
					if( (type == 1 && !sd->status.inventory[i].equip)
						|| (type == 2 && sd->status.inventory[i].equip)
						||  type == 3)
					{
						ARR_FIND( 0, MAX_INVENTORY, k, eq_n[k] <= 0 );
						if( k < MAX_INVENTORY )
							eq_n[k] = i;

						eq_num++;
					}
				}
				if(eq_num > 0){
					int n = eq_n[rand()%eq_num];
					if(rand()%10000 < per){
						if(sd->status.inventory[n].equip)
							pc_unequipitem(sd,n,3);
						pc_dropitem(sd,n,1);
					}
				}
			}
			else if(id > 0){
				for(i=0;i<MAX_INVENTORY;i++){
					if(sd->status.inventory[i].nameid == id
						&& rand()%10000 < per
						&& ((type == 1 && !sd->status.inventory[i].equip)
							|| (type == 2 && sd->status.inventory[i].equip)
							|| type == 3) ){
						if(sd->status.inventory[i].equip)
							pc_unequipitem(sd,i,3);
						pc_dropitem(sd,i,1);
						break;
					}
				}
			}
		}
	}
	// pvp
	// disable certain pvp functions on pk_mode [Valaris]
	if( map[sd->bl.m].flag.gvg_dungeon || (map[sd->bl.m].flag.pvp && !battle_config.pk_mode && !map[sd->bl.m].flag.pvp_nocalcrank) )
	{ // Pvp points always take effect on gvg_dungeon maps.
		sd->pvp_point -= 5;
		sd->pvp_lost++;
		if( src && src->type == BL_PC )
		{
			struct map_session_data *ssd = (struct map_session_data *)src;
			ssd->pvp_point++;
			ssd->pvp_won++;
		}
		if( sd->pvp_point < 0 )
		{
			add_timer(tick+1000, pc_respawn_timer,sd->bl.id,0);
			return 1|8;
		}
	}
	//GvG
	if( map_flag_gvg(sd->bl.m) )
	{
		add_timer(tick+1000, pc_respawn_timer, sd->bl.id, 0);
		return 1|8;
	}
	else if( sd->state.bg_id )
	{
		struct battleground_data *bg = bg_team_search(sd->state.bg_id);
		if( bg && bg->mapindex > 0 )
		{ // Respawn by BG
			add_timer(tick+1000, pc_respawn_timer, sd->bl.id, 0);
			return 1|8;
		}
	}


	//Reset "can log out" tick.
	if( battle_config.prevent_logout )
		sd->canlog_tick = gettick() - battle_config.prevent_logout;
	return 1;
}

void pc_revive(struct map_session_data *sd,unsigned int hp, unsigned int sp)
{
	if(hp) clif_updatestatus(sd,SP_HP);
	if(sp) clif_updatestatus(sd,SP_SP);

	pc_setstand(sd);
	if(battle_config.pc_invincible_time > 0)
		pc_setinvincibletimer(sd, battle_config.pc_invincible_time);
}
// script? A
//
/*==========================================
 * scriptpPCXe?^X?Ýoµ
 *------------------------------------------*/
int pc_readparam(struct map_session_data* sd,int type)
{
	int val = 0;

	nullpo_retr(0, sd);

	switch(type) {
	case SP_SKILLPOINT:  val = sd->status.skill_point; break;
	case SP_STATUSPOINT: val = sd->status.status_point; break;
	case SP_ZENY:        val = sd->status.zeny; break;
	case SP_BASELEVEL:   val = sd->status.base_level; break;
	case SP_JOBLEVEL:    val = sd->status.job_level; break;
	case SP_CLASS:       val = sd->status.class_; break;
	case SP_BASEJOB:     val = pc_mapid2jobid(sd->class_&MAPID_UPPERMASK, sd->status.sex); break; //Base job, extracting upper type.
	case SP_UPPER:       val = sd->class_&JOBL_UPPER?1:(sd->class_&JOBL_BABY?2:0); break;
	case SP_BASECLASS:   val = pc_mapid2jobid(sd->class_&MAPID_BASEMASK, sd->status.sex); break; //Extract base class tree. [Skotlex]
	case SP_SEX:         val = sd->status.sex; break;
	case SP_WEIGHT:      val = sd->weight; break;
	case SP_MAXWEIGHT:   val = sd->max_weight; break;
	case SP_BASEEXP:     val = sd->status.base_exp; break;
	case SP_JOBEXP:      val = sd->status.job_exp; break;
	case SP_NEXTBASEEXP: val = pc_nextbaseexp(sd); break;
	case SP_NEXTJOBEXP:  val = pc_nextjobexp(sd); break;
	case SP_HP:          val = sd->battle_status.hp; break;
	case SP_MAXHP:       val = sd->battle_status.max_hp; break;
	case SP_SP:          val = sd->battle_status.sp; break;
	case SP_MAXSP:       val = sd->battle_status.max_sp; break;
	case SP_STR:         val = sd->status.str; break;
	case SP_AGI:         val = sd->status.agi; break;
	case SP_VIT:         val = sd->status.vit; break;
	case SP_INT:         val = sd->status.int_; break;
	case SP_DEX:         val = sd->status.dex; break;
	case SP_LUK:         val = sd->status.luk; break;
	case SP_KARMA:       val = sd->status.karma; break;
	case SP_MANNER:      val = sd->status.manner; break;
	case SP_FAME:        val = sd->status.fame; break;
	}

	return val;
}

/*==========================================
 * scriptpPCXe?^XÝè
 *------------------------------------------*/
int pc_setparam(struct map_session_data *sd,int type,int val)
{
	int i = 0;

	nullpo_retr(0, sd);

	switch(type){
	case SP_BASELEVEL:
		if ((unsigned int)val > pc_maxbaselv(sd)) //Capping to max
			val = pc_maxbaselv(sd);
		if ((unsigned int)val > sd->status.base_level) {
			int stat=0;
			for (i = 1; i <= (int)((unsigned int)val - sd->status.base_level); i++)
				stat += (sd->status.base_level + i + 14) / 5 ;
			sd->status.status_point += stat;
		}
		sd->status.base_level = (unsigned int)val;
		sd->status.base_exp = 0;
		clif_updatestatus(sd, SP_BASELEVEL);
		clif_updatestatus(sd, SP_NEXTBASEEXP);
		clif_updatestatus(sd, SP_STATUSPOINT);
		clif_updatestatus(sd, SP_BASEEXP);
		status_calc_pc(sd, 0);
		break;
	case SP_JOBLEVEL:
		if ((unsigned int)val >= sd->status.job_level) {
			if ((unsigned int)val > pc_maxjoblv(sd)) val = pc_maxjoblv(sd);
			sd->status.skill_point += val - sd->status.job_level;
			clif_updatestatus(sd, SP_SKILLPOINT);
		}
		sd->status.job_level = (unsigned int)val;
		sd->status.job_exp = 0;
		clif_updatestatus(sd, SP_JOBLEVEL);
		clif_updatestatus(sd, SP_NEXTJOBEXP);
		clif_updatestatus(sd, SP_JOBEXP);
		status_calc_pc(sd, 0);
		clif_updatestatus(sd,type);
		break;
	case SP_SKILLPOINT:
		sd->status.skill_point = val;
		break;
	case SP_STATUSPOINT:
		sd->status.status_point = val;
		break;
	case SP_ZENY:
		if( val < 0 )
			return 0;// can't set negative zeny
		sd->status.zeny = cap_value(val, 0, MAX_ZENY);
		break;
	case SP_BASEEXP:
		if(pc_nextbaseexp(sd) > 0) {
			sd->status.base_exp = val;
			pc_checkbaselevelup(sd);
		}
		break;
	case SP_JOBEXP:
		if(pc_nextjobexp(sd) > 0) {
			sd->status.job_exp = val;
			pc_checkjoblevelup(sd);
		}
		break;
	case SP_SEX:
		sd->status.sex = val;
		break;
	case SP_WEIGHT:
		sd->weight = val;
		break;
	case SP_MAXWEIGHT:
		sd->max_weight = val;
		break;
	case SP_HP:
		sd->battle_status.hp = val;
		break;
	case SP_MAXHP:
		sd->battle_status.max_hp = val;
		break;
	case SP_SP:
		sd->battle_status.sp = val;
		break;
	case SP_MAXSP:
		sd->battle_status.max_sp = val;
		break;
	case SP_STR:
		sd->status.str = val;
		break;
	case SP_AGI:
		sd->status.agi = val;
		break;
	case SP_VIT:
		sd->status.vit = val;
		break;
	case SP_INT:
		sd->status.int_ = val;
		break;
	case SP_DEX:
		sd->status.dex = val;
		break;
	case SP_LUK:
		sd->status.luk = val;
		break;
	case SP_KARMA:
		sd->status.karma = val;
		break;
	case SP_MANNER:
		sd->status.manner = val;
		break;
	case SP_FAME:
		sd->status.fame = val;
		break;
	}
	clif_updatestatus(sd,type);

	return 1;
}

/*==========================================
 * HP/SP Healing. If flag is passed, the heal type is through clif_heal, otherwise update status.
 *------------------------------------------*/
void pc_heal(struct map_session_data *sd,unsigned int hp,unsigned int sp, int type)
{
	if (type) {
		if (hp)
			clif_heal(sd->fd,SP_HP,hp);
		if (sp)
			clif_heal(sd->fd,SP_SP,sp);
	} else {
		if(hp)
			clif_updatestatus(sd,SP_HP);
		if(sp)
			clif_updatestatus(sd,SP_SP);
	}
	return;
}

/*==========================================
 * HP/SPñ
 *------------------------------------------*/
int pc_itemheal(struct map_session_data *sd,int itemid, int hp,int sp)
{
	int i, bonus;

	if(hp) {
		bonus = 100 + (sd->battle_status.vit<<1)
			+ pc_checkskill(sd,SM_RECOVERY)*10
			+ pc_checkskill(sd,AM_LEARNINGPOTION)*5;
		// A potion produced by an Alchemist in the Fame Top 10 gets +50% effect [DracoRPG]
		if (potion_flag > 1)
			bonus += bonus*(potion_flag-1)*50/100;
		//Item Group bonuses
		bonus += bonus*itemdb_group_bonus(sd, itemid)/100;
		//Individual item bonuses.
		for(i = 0; i < ARRAYLENGTH(sd->itemhealrate) && sd->itemhealrate[i].nameid; i++)
		{
			if (sd->itemhealrate[i].nameid == itemid) {
				bonus += bonus*sd->itemhealrate[i].rate/100;
				break;
			}
		}
		if(bonus!=100)
			hp = hp * bonus / 100;

		// Recovery Potion
		if( sd->sc.data[SC_INCHEALRATE] )
			hp += (int)(hp * sd->sc.data[SC_INCHEALRATE]->val1/100.);
	}
	if(sp) {
		bonus = 100 + (sd->battle_status.int_<<1)
			+ pc_checkskill(sd,MG_SRECOVERY)*10
			+ pc_checkskill(sd,AM_LEARNINGPOTION)*5;
		if (potion_flag > 1)
			bonus += bonus*(potion_flag-1)*50/100;
		if(bonus != 100)
			sp = sp * bonus / 100;
	}

	if (sd->sc.data[SC_CRITICALWOUND])
	{
		hp -= hp * sd->sc.data[SC_CRITICALWOUND]->val2 / 100;
		sp -= sp * sd->sc.data[SC_CRITICALWOUND]->val2 / 100;
	}

	return status_heal(&sd->bl, hp, sp, 1);
}

/*==========================================
 * HP/SPñ
 *------------------------------------------*/
int pc_percentheal(struct map_session_data *sd,int hp,int sp)
{
	nullpo_retr(0, sd);

	if(hp > 100) hp = 100;
	else
	if(hp <-100) hp =-100;

	if(sp > 100) sp = 100;
	else
	if(sp <-100) sp =-100;

	if(hp >= 0 && sp >= 0) //Heal
		return status_percent_heal(&sd->bl, hp, sp);

	if(hp <= 0 && sp <= 0) //Damage (negative rates indicate % of max rather than current), and only kill target IF the specified amount is 100%
		return status_percent_damage(NULL, &sd->bl, hp, sp, hp==-100);

	//Crossed signs
	if(hp) {
		if(hp > 0)
			status_percent_heal(&sd->bl, hp, 0);
		else
			status_percent_damage(NULL, &sd->bl, hp, 0, hp==-100);
	}
	
	if(sp) {
		if(sp > 0)
			status_percent_heal(&sd->bl, 0, sp);
		else
			status_percent_damage(NULL, &sd->bl, 0, sp, false);
	}
	return 0;
}

/*==========================================
 * E?X
 * ø?	job EÆ 0`23
 *		upper Êí 0, ?¶ 1, {q 2, »ÌÜÜ -1
 * Rewrote to make it tidider [Celest]
 *------------------------------------------*/
int pc_jobchange(struct map_session_data *sd,int job, int upper)
{
	int i, fame_flag=0;
	int b_class;

	nullpo_retr(0, sd);

	if (job < 0)
		return 1;

	//Normalize job.
	b_class = pc_jobid2mapid(job);
	if (b_class == -1)
		return 1;
	switch (upper) {
		case 1:
			b_class|= JOBL_UPPER; 
			break;
		case 2:
			b_class|= JOBL_BABY;
			break;
	}
	//This will automatically adjust bard/dancer classes to the correct gender
	//That is, if you try to jobchange into dancer, it will turn you to bard.	
	job = pc_mapid2jobid(b_class, sd->status.sex);
	if (job == -1)
		return 1;
	
	if ((unsigned short)b_class == sd->class_)
		return 1; //Nothing to change.
	// check if we are changing from 1st to 2nd job
	if (b_class&JOBL_2) {
		if (!(sd->class_&JOBL_2))
			sd->change_level = sd->status.job_level;
		else if (!sd->change_level)
			sd->change_level = 40; //Assume 40?
		pc_setglobalreg (sd, "jobchange_level", sd->change_level);
	}

	if(sd->cloneskill_id) {
		sd->cloneskill_id = 0;
		pc_setglobalreg(sd, "CLONE_SKILL", 0);
		pc_setglobalreg(sd, "CLONE_SKILL_LV", 0);
	}
	if ((b_class&&MAPID_UPPERMASK) != (sd->class_&MAPID_UPPERMASK))
	{ //Things to remove when changing class tree.
		const int class_ = pc_class2idx(sd->status.class_);
		short id;
		for(i = 0; i < MAX_SKILL_TREE && (id = skill_tree[class_][i].id) > 0; i++) {
			//Remove status specific to your current tree skills.
			enum sc_type sc = status_skill2sc(id);
			if (sc > SC_COMMON_MAX && sd->sc.data[sc])
				status_change_end(&sd->bl, sc, -1);
		}
	}
	
	sd->status.class_ = job;
	fame_flag = pc_famerank(sd->status.char_id,sd->class_&MAPID_UPPERMASK);
	sd->class_ = (unsigned short)b_class;
	sd->status.job_level=1;
	sd->status.job_exp=0;
	clif_updatestatus(sd,SP_JOBLEVEL);
	clif_updatestatus(sd,SP_JOBEXP);
	clif_updatestatus(sd,SP_NEXTJOBEXP);

	for(i=0;i<EQI_MAX;i++) {
		if(sd->equip_index[i] >= 0)
			if(!pc_isequip(sd,sd->equip_index[i]))
				pc_unequipitem(sd,sd->equip_index[i],2);	// ?õOµ
	}

	//Change look, if disguised, you need to undisguise 
	//to correctly calculate new job sprite without
	if (sd->disguise)
		pc_disguise(sd, 0);

	status_set_viewdata(&sd->bl, job);
	clif_changelook(&sd->bl,LOOK_BASE,sd->vd.class_); // move sprite update to prevent client crashes with incompatible equipment [Valaris]
	if(sd->vd.cloth_color)
		clif_changelook(&sd->bl,LOOK_CLOTHES_COLOR,sd->vd.cloth_color);

	//Update skill tree.
	pc_calc_skilltree(sd);
	clif_skillinfoblock(sd);

	//Remove peco/cart/falcon
	i = sd->sc.option;
	if(i&OPTION_RIDING && !pc_checkskill(sd, KN_RIDING))
		i&=~OPTION_RIDING;
	if(i&OPTION_CART && !pc_checkskill(sd, MC_PUSHCART))
		i&=~OPTION_CART;
	if(i&OPTION_FALCON && !pc_checkskill(sd, HT_FALCON))
		i&=~OPTION_FALCON;

	if(i != sd->sc.option)
		pc_setoption(sd, i);

	if(merc_is_hom_active(sd->hd) && !pc_checkskill(sd, AM_CALLHOMUN))
		merc_hom_vaporize(sd, 0);
	
	if(sd->status.manner < 0)
		clif_changestatus(&sd->bl,SP_MANNER,sd->status.manner);

	status_calc_pc(sd,0);
	pc_checkallowskill(sd);
	pc_equiplookall(sd);

	//if you were previously famous, not anymore.
	if (fame_flag) {
		chrif_save(sd,0);
		chrif_buildfamelist();
	} else if (sd->status.fame > 0) {
		//It may be that now they are famous?
 		switch (sd->class_&MAPID_UPPERMASK) {
			case MAPID_BLACKSMITH:
			case MAPID_ALCHEMIST:
			case MAPID_TAEKWON:
				chrif_save(sd,0);
				chrif_buildfamelist();
			break;
		}
	}

	return 0;
}

/*==========================================
 * ©½Ú?X
 *------------------------------------------*/
int pc_equiplookall(struct map_session_data *sd)
{
	nullpo_retr(0, sd);

#if PACKETVER < 4
	clif_changelook(&sd->bl,LOOK_WEAPON,sd->status.weapon);
	clif_changelook(&sd->bl,LOOK_SHIELD,sd->status.shield);
#else
	clif_changelook(&sd->bl,LOOK_WEAPON,0);
	clif_changelook(&sd->bl,LOOK_SHOES,0);
#endif
	clif_changelook(&sd->bl,LOOK_HEAD_BOTTOM,sd->status.head_bottom);
	clif_changelook(&sd->bl,LOOK_HEAD_TOP,sd->status.head_top);
	clif_changelook(&sd->bl,LOOK_HEAD_MID,sd->status.head_mid);

	return 0;
}

/*==========================================
 * ©½Ú?X
 *------------------------------------------*/
int pc_changelook(struct map_session_data *sd,int type,int val)
{
	nullpo_retr(0, sd);

	switch(type){
	case LOOK_HAIR:	//Use the battle_config limits! [Skotlex]
		if (val < battle_config.min_hair_style)
			val = battle_config.min_hair_style;
		else if (val > battle_config.max_hair_style)
			val = battle_config.max_hair_style;
		if (sd->status.hair != val)
		{
			sd->status.hair=val;
			if (sd->status.guild_id) //Update Guild Window. [Skotlex]
				intif_guild_change_memberinfo(sd->status.guild_id,sd->status.account_id,sd->status.char_id,
				GMI_HAIR,&sd->status.hair,sizeof(sd->status.hair));
		}
		break;
	case LOOK_WEAPON:
		sd->status.weapon=val;
		break;
	case LOOK_HEAD_BOTTOM:
		sd->status.head_bottom=val;
		break;
	case LOOK_HEAD_TOP:
		sd->status.head_top=val;
		break;
	case LOOK_HEAD_MID:
		sd->status.head_mid=val;
		break;
	case LOOK_HAIR_COLOR:	//Use the battle_config limits! [Skotlex]
		if (val < battle_config.min_hair_color)
			val = battle_config.min_hair_color;
		else if (val > battle_config.max_hair_color)
			val = battle_config.max_hair_color;
		if (sd->status.hair_color != val)
		{
			sd->status.hair_color=val;
			if (sd->status.guild_id) //Update Guild Window. [Skotlex]
				intif_guild_change_memberinfo(sd->status.guild_id,sd->status.account_id,sd->status.char_id,
				GMI_HAIR_COLOR,&sd->status.hair_color,sizeof(sd->status.hair_color));
		}
		break;
	case LOOK_CLOTHES_COLOR:	//Use the battle_config limits! [Skotlex]
		if (val < battle_config.min_cloth_color)
			val = battle_config.min_cloth_color;
		else if (val > battle_config.max_cloth_color)
			val = battle_config.max_cloth_color;
		sd->status.clothes_color=val;
		break;
	case LOOK_SHIELD:
		sd->status.shield=val;
		break;
	case LOOK_SHOES:
		break;
	}
	clif_changelook(&sd->bl,type,val);
	return 0;
}

/*==========================================
 * t?i(é,yR,J?g)Ýè
 *------------------------------------------*/
int pc_setoption(struct map_session_data *sd,int type)
{
	int p_type, new_look=0;
	nullpo_retr(0, sd);
	p_type = sd->sc.option;

	//Option has to be changed client-side before the class sprite or it won't always work (eg: Wedding sprite) [Skotlex]
	sd->sc.option=type;
	clif_changeoption(&sd->bl);

	if (type&OPTION_RIDING && !(p_type&OPTION_RIDING) && (sd->class_&MAPID_BASEMASK) == MAPID_SWORDMAN)
	{	//We are going to mount. [Skotlex]
		clif_status_load(&sd->bl,SI_RIDING,1);
		status_calc_pc(sd,0); //Mounting/Umounting affects walk and attack speeds.
	}
	else if (!(type&OPTION_RIDING) && p_type&OPTION_RIDING && (sd->class_&MAPID_BASEMASK) == MAPID_SWORDMAN)
	{	//We are going to dismount.
		clif_status_load(&sd->bl,SI_RIDING,0);
		status_calc_pc(sd,0); //Mounting/Umounting affects walk and attack speeds.
	}

	if(type&OPTION_CART && !(p_type&OPTION_CART))
  	{ //Cart On
		clif_cartlist(sd);
		clif_updatestatus(sd, SP_CARTINFO);
		if(pc_checkskill(sd, MC_PUSHCART) < 10)
			status_calc_pc(sd,0); //Apply speed penalty.
	} else
	if(!(type&OPTION_CART) && p_type&OPTION_CART)
	{ //Cart Off
		clif_clearcart(sd->fd);
		if(pc_checkskill(sd, MC_PUSHCART) < 10)
			status_calc_pc(sd,0); //Remove speed penalty.
	}

	if (type&OPTION_FALCON && !(p_type&OPTION_FALCON)) //Falcon ON
		clif_status_load(&sd->bl,SI_FALCON,1);
	else if (!(type&OPTION_FALCON) && p_type&OPTION_FALCON) //Falcon OFF
		clif_status_load(&sd->bl,SI_FALCON,0);

	if (type&OPTION_FLYING && !(p_type&OPTION_FLYING))
		new_look = JOB_STAR_GLADIATOR2;
	else if (!(type&OPTION_FLYING) && p_type&OPTION_FLYING)
		new_look = -1;
	
	if (type&OPTION_WEDDING && !(p_type&OPTION_WEDDING))
		new_look = JOB_WEDDING;
	else if (!(type&OPTION_WEDDING) && p_type&OPTION_WEDDING)
		new_look = -1;

	if (type&OPTION_XMAS && !(p_type&OPTION_XMAS))
		new_look = JOB_XMAS;
	else if (!(type&OPTION_XMAS) && p_type&OPTION_XMAS)
		new_look = -1;

	if (type&OPTION_SUMMER && !(p_type&OPTION_SUMMER))
		new_look = JOB_SUMMER;
	else if (!(type&OPTION_SUMMER) && p_type&OPTION_SUMMER)
		new_look = -1;

	if (sd->disguise || !new_look)
		return 0; //Disguises break sprite changes

	if (new_look < 0) { //Restore normal look.
		status_set_viewdata(&sd->bl, sd->status.class_);
		new_look = sd->vd.class_;
	}

	pc_stop_attack(sd); //Stop attacking on new view change (to prevent wedding/santa attacks.
	clif_changelook(&sd->bl,LOOK_BASE,new_look);
	if (sd->vd.cloth_color)
		clif_changelook(&sd->bl,LOOK_CLOTHES_COLOR,sd->vd.cloth_color);
	clif_skillinfoblock(sd); // Skill list needs to be updated after base change.

	return 0;
}

/*==========================================
 * J?gÝè
 *------------------------------------------*/
int pc_setcart(struct map_session_data *sd,int type)
{
	int cart[6] = {0x0000,OPTION_CART1,OPTION_CART2,OPTION_CART3,OPTION_CART4,OPTION_CART5};
	int option;

	nullpo_retr(0, sd);

	if( type < 0 || type > 5 )
		return 1;// Never trust the values sent by the client! [Skotlex]

	if( pc_checkskill(sd,MC_PUSHCART) <= 0 )
		return 1;// Push cart is required

	// Update option
	option = sd->sc.option;
	option &= ~OPTION_CART;// clear cart bits
	option |= cart[type]; // set cart
	pc_setoption(sd, option);

	return 0;
}

/*==========================================
 * éÝè
 *------------------------------------------*/
int pc_setfalcon(TBL_PC* sd, int flag)
{
	if( flag ){
		if( pc_checkskill(sd,HT_FALCON)>0 )	// t@R}X^?XL
			pc_setoption(sd,sd->sc.option|OPTION_FALCON);
	} else if( pc_isfalcon(sd) ){
		pc_setoption(sd,sd->sc.option&~OPTION_FALCON); // remove falcon
	}

	return 0;
}

/*==========================================
 * yRyRÝè
 *------------------------------------------*/
int pc_setriding(TBL_PC* sd, int flag)
{
	if( flag ){
		if( pc_checkskill(sd,KN_RIDING) > 0 ) // CfBOXL
			pc_setoption(sd, sd->sc.option|OPTION_RIDING);
	} else if( pc_isriding(sd) ){
		pc_setoption(sd, sd->sc.option&~OPTION_RIDING);
	}

	return 0;
}

/*==========================================
 * ACehbvÂsÂ»è
 *------------------------------------------*/
int pc_candrop(struct map_session_data *sd,struct item *item)
{
	int level = pc_isGM(sd);
	if( item && item->expire_time )
		return 0;
	if( !pc_can_give_items(level) ) //check if this GM level can drop items
		return 0;
	return (itemdb_isdropable(item, level));
}

/*==========================================
 * scriptp??Ìlð?Þ
 *------------------------------------------*/
int pc_readreg(struct map_session_data* sd, int reg)
{
	int i;

	nullpo_retr(0, sd);

	ARR_FIND( 0, sd->reg_num, i,  sd->reg[i].index == reg );
	return ( i < sd->reg_num ) ? sd->reg[i].data : 0;
}
/*==========================================
 * scriptp??ÌlðÝè
 *------------------------------------------*/
int pc_setreg(struct map_session_data* sd, int reg, int val)
{
	int i;

	nullpo_retr(0, sd);

	ARR_FIND( 0, sd->reg_num, i, sd->reg[i].index == reg );
	if( i < sd->reg_num )
	{// overwrite existing entry
		sd->reg[i].data = val;
		return 1;
	}

	ARR_FIND( 0, sd->reg_num, i, sd->reg[i].data == 0 );
	if( i == sd->reg_num )
	{// nothing free, increase size
		sd->reg_num++;
		RECREATE(sd->reg, struct script_reg, sd->reg_num);
	}
	sd->reg[i].index = reg;
	sd->reg[i].data = val;

	return 1;
}

/*==========================================
 * scriptp¶ñ??Ìlð?Þ
 *------------------------------------------*/
char* pc_readregstr(struct map_session_data* sd, int reg)
{
	int i;

	nullpo_retr(0, sd);

	ARR_FIND( 0, sd->regstr_num, i,  sd->regstr[i].index == reg );
	return ( i < sd->regstr_num ) ? sd->regstr[i].data : NULL;
}
/*==========================================
 * scriptp¶ñ??ÌlðÝè
 *------------------------------------------*/
int pc_setregstr(struct map_session_data* sd, int reg, const char* str)
{
	int i;

	nullpo_retr(0, sd);

	ARR_FIND( 0, sd->regstr_num, i, sd->regstr[i].index == reg );
	if( i < sd->regstr_num )
	{// found entry, update
		if( str == NULL || *str == '\0' )
		{// empty string
			if( sd->regstr[i].data != NULL )
				aFree(sd->regstr[i].data);
			sd->regstr[i].data = NULL;
		}
		else if( sd->regstr[i].data )
		{// recreate
			size_t len = strlen(str)+1;
			RECREATE(sd->regstr[i].data, char, len);
			memcpy(sd->regstr[i].data, str, len*sizeof(char));
		}
		else
		{// create
			sd->regstr[i].data = aStrdup(str);
		}
		return 1;
	}

	if( str == NULL || *str == '\0' )
		return 1;// nothing to add, empty string

	ARR_FIND( 0, sd->regstr_num, i, sd->regstr[i].data == NULL );
	if( i == sd->regstr_num )
	{// nothing free, increase size
		sd->regstr_num++;
		RECREATE(sd->regstr, struct script_regstr, sd->regstr_num);
	}
	sd->regstr[i].index = reg;
	sd->regstr[i].data = aStrdup(str);

	return 1;
}

int pc_readregistry(struct map_session_data *sd,const char *reg,int type)
{
	struct global_reg *sd_reg;
	int i,max;

	nullpo_retr(0, sd);
	switch (type) {
	case 3: //Char reg
		sd_reg = sd->save_reg.global;
		max = sd->save_reg.global_num;
	break;
	case 2: //Account reg
		sd_reg = sd->save_reg.account;
		max = sd->save_reg.account_num;
	break;
	case 1: //Account2 reg
		sd_reg = sd->save_reg.account2;
		max = sd->save_reg.account2_num;
	break;
	default:
		return 0;
	}
	if (max == -1) {
		ShowError("pc_readregistry: Trying to read reg value %s (type %d) before it's been loaded!\n", reg, type);
		//This really shouldn't happen, so it's possible the data was lost somewhere, we should request it again.
		intif_request_registry(sd,type==3?4:type);
		return 0;
	}

	ARR_FIND( 0, max, i, strcmp(sd_reg[i].str,reg) == 0 );
	return ( i < max ) ? atoi(sd_reg[i].value) : 0;
}

char* pc_readregistry_str(struct map_session_data *sd,const char *reg,int type)
{
	struct global_reg *sd_reg;
	int i,max;
	
	nullpo_retr(0, sd);
	switch (type) {
	case 3: //Char reg
		sd_reg = sd->save_reg.global;
		max = sd->save_reg.global_num;
	break;
	case 2: //Account reg
		sd_reg = sd->save_reg.account;
		max = sd->save_reg.account_num;
	break;
	case 1: //Account2 reg
		sd_reg = sd->save_reg.account2;
		max = sd->save_reg.account2_num;
	break;
	default:
		return NULL;
	}
	if (max == -1) {
		ShowError("pc_readregistry: Trying to read reg value %s (type %d) before it's been loaded!\n", reg, type);
		//This really shouldn't happen, so it's possible the data was lost somewhere, we should request it again.
		intif_request_registry(sd,type==3?4:type);
		return NULL;
	}

	ARR_FIND( 0, max, i, strcmp(sd_reg[i].str,reg) == 0 );
	return ( i < max ) ? sd_reg[i].value : NULL;
}

int pc_setregistry(struct map_session_data *sd,const char *reg,int val,int type)
{
	struct global_reg *sd_reg;
	int i,*max, regmax;

	nullpo_retr(0, sd);

	switch( type )
	{
	case 3: //Char reg
		if( !strcmp(reg,"PC_DIE_COUNTER") && sd->die_counter != val )
		{
			i = (!sd->die_counter && (sd->class_&MAPID_UPPERMASK) == MAPID_SUPER_NOVICE);
			sd->die_counter = val;
			if( i )
				status_calc_pc(sd,0); // Lost the bonus.
		}
		else if( !strcmp(reg,"COOK_MASTERY") && sd->cook_mastery != val )
		{
			val = cap_value(val, 0, 1999);
			sd->cook_mastery = val;
		}
		sd_reg = sd->save_reg.global;
		max = &sd->save_reg.global_num;
		regmax = GLOBAL_REG_NUM;
	break;
	case 2: //Account reg
		if( !strcmp(reg,"#CASHPOINTS") && sd->cashPoints != val )
		{
			val = cap_value(val, 0, MAX_ZENY);
			sd->cashPoints = val;
		}
		else if( !strcmp(reg,"#KAFRAPOINTS") && sd->kafraPoints != val )
		{
			val = cap_value(val, 0, MAX_ZENY);
			sd->kafraPoints = val;
		}
		sd_reg = sd->save_reg.account;
		max = &sd->save_reg.account_num;
		regmax = ACCOUNT_REG_NUM;
	break;
	case 1: //Account2 reg
		sd_reg = sd->save_reg.account2;
		max = &sd->save_reg.account2_num;
		regmax = ACCOUNT_REG2_NUM;
	break;
	default:
		return 0;
	}
	if (*max == -1) {
		ShowError("pc_setregistry : refusing to set %s (type %d) until vars are received.\n", reg, type);
		return 1;
	}
	
	// delete reg
	if (val == 0) {
		ARR_FIND( 0, *max, i, strcmp(sd_reg[i].str, reg) == 0 );
		if( i < *max )
		{
			if (i != *max - 1)
				memcpy(&sd_reg[i], &sd_reg[*max - 1], sizeof(struct global_reg));
			memset(&sd_reg[*max - 1], 0, sizeof(struct global_reg));
			(*max)--;
			sd->state.reg_dirty |= 1<<(type-1); //Mark this registry as "need to be saved"
		}
		return 1;
	}
	// change value if found
	ARR_FIND( 0, *max, i, strcmp(sd_reg[i].str, reg) == 0 );
	if( i < *max )
	{
		safesnprintf(sd_reg[i].value, sizeof(sd_reg[i].value), "%d", val);
		sd->state.reg_dirty |= 1<<(type-1);
		return 1;
	}

	// add value if not found
	if (i < regmax) {
		memset(&sd_reg[i], 0, sizeof(struct global_reg));
		safestrncpy(sd_reg[i].str, reg, sizeof(sd_reg[i].str));
		safesnprintf(sd_reg[i].value, sizeof(sd_reg[i].value), "%d", val);
		(*max)++;
		sd->state.reg_dirty |= 1<<(type-1);
		return 1;
	}

	ShowError("pc_setregistry : couldn't set %s, limit of registries reached (%d)\n", reg, regmax);

	return 0;
}

int pc_setregistry_str(struct map_session_data *sd,const char *reg,const char *val,int type)
{
	struct global_reg *sd_reg;
	int i,*max, regmax;

	nullpo_retr(0, sd);
	if (reg[strlen(reg)-1] != '$') {
		ShowError("pc_setregistry_str : reg %s must be string (end in '$') to use this!\n", reg);
		return 0;
	}

	switch (type) {
	case 3: //Char reg
		sd_reg = sd->save_reg.global;
		max = &sd->save_reg.global_num;
		regmax = GLOBAL_REG_NUM;
	break;
	case 2: //Account reg
		sd_reg = sd->save_reg.account;
		max = &sd->save_reg.account_num;
		regmax = ACCOUNT_REG_NUM;
	break;
	case 1: //Account2 reg
		sd_reg = sd->save_reg.account2;
		max = &sd->save_reg.account2_num;
		regmax = ACCOUNT_REG2_NUM;
	break;
	default:
		return 0;
	}
	if (*max == -1) {
		ShowError("pc_setregistry_str : refusing to set %s (type %d) until vars are received.\n", reg, type);
		return 0;
	}
	
	// delete reg
	if (!val || strcmp(val,"")==0)
	{
		ARR_FIND( 0, *max, i, strcmp(sd_reg[i].str, reg) == 0 );
		if( i < *max )
		{
			if (i != *max - 1)
				memcpy(&sd_reg[i], &sd_reg[*max - 1], sizeof(struct global_reg));
			memset(&sd_reg[*max - 1], 0, sizeof(struct global_reg));
			(*max)--;
			sd->state.reg_dirty |= 1<<(type-1); //Mark this registry as "need to be saved"
			if (type!=3) intif_saveregistry(sd,type);
		}
		return 1;
	}

	// change value if found
	ARR_FIND( 0, *max, i, strcmp(sd_reg[i].str, reg) == 0 );
	if( i < *max )
	{
		safestrncpy(sd_reg[i].value, val, sizeof(sd_reg[i].value));
		sd->state.reg_dirty |= 1<<(type-1); //Mark this registry as "need to be saved"
		if (type!=3) intif_saveregistry(sd,type);
		return 1;
	}

	// add value if not found
	if (i < regmax) {
		memset(&sd_reg[i], 0, sizeof(struct global_reg));
		safestrncpy(sd_reg[i].str, reg, sizeof(sd_reg[i].str));
		safestrncpy(sd_reg[i].value, val, sizeof(sd_reg[i].value));
		(*max)++;
		sd->state.reg_dirty |= 1<<(type-1); //Mark this registry as "need to be saved"
		if (type!=3) intif_saveregistry(sd,type);
		return 1;
	}

	ShowError("pc_setregistry : couldn't set %s, limit of registries reached (%d)\n", reg, regmax);

	return 0;
}

/*==========================================
 * Cxg^C}??
 *------------------------------------------*/
static int pc_eventtimer(int tid, unsigned int tick, int id, intptr data)
{
	struct map_session_data *sd=map_id2sd(id);
	char *p = (char *)data;
	int i;
	if(sd==NULL)
		return 0;

	ARR_FIND( 0, MAX_EVENTTIMER, i, sd->eventtimer[i] == tid );
	if( i < MAX_EVENTTIMER )
	{
		sd->eventtimer[i] = -1;
		sd->eventcount--;
		npc_event(sd,p,0);
	}
	else
		ShowError("pc_eventtimer: no such event timer\n");

	if (p) aFree(p);
	return 0;
}

/*==========================================
 * Cxg^C}?ÇÁ
 *------------------------------------------*/
int pc_addeventtimer(struct map_session_data *sd,int tick,const char *name)
{
	int i;
	nullpo_retr(0, sd);

	ARR_FIND( 0, MAX_EVENTTIMER, i, sd->eventtimer[i] == -1 );
	if( i == MAX_EVENTTIMER )
		return 0;

	sd->eventtimer[i] = add_timer(gettick()+tick, pc_eventtimer, sd->bl.id, (intptr)aStrdup(name));
	sd->eventcount++;

	return 1;
}

/*==========================================
 * Cxg^C}?í
 *------------------------------------------*/
int pc_deleventtimer(struct map_session_data *sd,const char *name)
{
	char* p = NULL;
	int i;

	nullpo_retr(0, sd);

	if (sd->eventcount <= 0)
		return 0;

	// find the named event timer
	ARR_FIND( 0, MAX_EVENTTIMER, i,
		sd->eventtimer[i] != -1 &&
		(p = (char *)(get_timer(sd->eventtimer[i])->data)) != NULL &&
		strcmp(p, name) == 0
	);
	if( i == MAX_EVENTTIMER )
		return 0; // not found

	delete_timer(sd->eventtimer[i],pc_eventtimer);
	sd->eventtimer[i]=-1;
	sd->eventcount--;
	aFree(p);

	return 1;
}

/*==========================================
 * Cxg^C}?JEglÇÁ
 *------------------------------------------*/
int pc_addeventtimercount(struct map_session_data *sd,const char *name,int tick)
{
	int i;

	nullpo_retr(0, sd);

	for(i=0;i<MAX_EVENTTIMER;i++)
		if( sd->eventtimer[i]!=-1 && strcmp(
			(char *)(get_timer(sd->eventtimer[i])->data), name)==0 ){
				addtick_timer(sd->eventtimer[i],tick);
				break;
		}

	return 0;
}

/*==========================================
 * Cxg^C}?Sí
 *------------------------------------------*/
int pc_cleareventtimer(struct map_session_data *sd)
{
	int i;

	nullpo_retr(0, sd);

	if (sd->eventcount <= 0)
		return 0;

	for(i=0;i<MAX_EVENTTIMER;i++)
		if( sd->eventtimer[i]!=-1 ){
			char *p = (char *)(get_timer(sd->eventtimer[i])->data);
			delete_timer(sd->eventtimer[i],pc_eventtimer);
			sd->eventtimer[i]=-1;
			sd->eventcount--;
			if (p) aFree(p);
		}
	return 0;
}

//
// ? õ¨
//
/*==========================================
 * ACeð?õ·é
 *------------------------------------------*/
int pc_equipitem(struct map_session_data *sd,int n,int req_pos)
{
	int i,pos,flag=0;
	struct item_data *id;

	nullpo_retr(0, sd);

	if( n < 0 || n >= MAX_INVENTORY ) {
		clif_equipitemack(sd,0,0,0);
		return 0;
	}

	if( DIFF_TICK(sd->canequip_tick,gettick()) > 0 )
	{
		clif_equipitemack(sd,n,0,0);
		return 0;
	}

	id = sd->inventory_data[n];
	pos = pc_equippoint(sd,n); //With a few exceptions, item should go in all specified slots.

	if(battle_config.battle_log)
		ShowInfo("equip %d(%d) %x:%x\n",sd->status.inventory[n].nameid,n,id->equip,req_pos);
	if(!pc_isequip(sd,n) || !(pos&req_pos) || sd->status.inventory[n].equip != 0 || sd->status.inventory[n].attribute==1 ) { // [Valaris]
		clif_equipitemack(sd,n,0,0);	// fail
		return 0;
	}

	if( sd->sc.data[SC_BERSERK] )
	{
		clif_equipitemack(sd,n,0,0);	// fail
		return 0;
	}

	if(pos == EQP_ACC) { //Accesories should only go in one of the two,
		pos = req_pos&EQP_ACC;
		if (pos == EQP_ACC) //User specified both slots.. 
			pos = sd->equip_index[EQI_ACC_R] >= 0 ? EQP_ACC_L : EQP_ACC_R;
	}

	if(pos == EQP_ARMS && id->equip == EQP_HAND_R)
	{	//Dual wield capable weapon.
	  	pos = (req_pos&EQP_ARMS);
		if (pos == EQP_ARMS) //User specified both slots, pick one for them.
			pos = sd->equip_index[EQI_HAND_R] >= 0 ? EQP_HAND_L : EQP_HAND_R;
	}

	if (pos&EQP_HAND_R && battle_config.use_weapon_skill_range&BL_PC)
	{	//Update skill-block range database when weapon range changes. [Skotlex]
		i = sd->equip_index[EQI_HAND_R];
		if (i < 0 || !sd->inventory_data[i]) //No data, or no weapon equipped
			flag = 1;
		else
			flag = id->range != sd->inventory_data[i]->range;
	}

	for(i=0;i<EQI_MAX;i++) {
		if(pos & equip_pos[i]) {
			if(sd->equip_index[i] >= 0) //Slot taken, remove item from there.
				pc_unequipitem(sd,sd->equip_index[i],2);

			sd->equip_index[i] = n;
		}
	}

	if(pos==EQP_AMMO){
		clif_arrowequip(sd,n);
		clif_arrow_fail(sd,3);
	}
	else
		clif_equipitemack(sd,n,pos,1);

	sd->status.inventory[n].equip=pos;

	if(pos & EQP_HAND_R) {
		if(id)
			sd->weapontype1 = id->look;
		else
			sd->weapontype1 = 0;
		pc_calcweapontype(sd);
		clif_changelook(&sd->bl,LOOK_WEAPON,sd->status.weapon);
	}
	if(pos & EQP_HAND_L) {
		if(id) {
			if(id->type == IT_WEAPON) {
				sd->status.shield = 0;
				if(sd->status.inventory[n].equip == EQP_HAND_L)
					sd->weapontype2 = id->look;
				else
					sd->weapontype2 = 0;
			}
			else
			if(id->type == IT_ARMOR) {
				sd->status.shield = id->look;
				sd->weapontype2 = 0;
			}
		}
		else
			sd->status.shield = sd->weapontype2 = 0;
		pc_calcweapontype(sd);
		clif_changelook(&sd->bl,LOOK_SHIELD,sd->status.shield);
	}
	//Added check to prevent sending the same look on multiple slots ->
	//causes client to redraw item on top of itself. (suggested by Lupus)
	if(pos & EQP_HEAD_LOW) {
		if(id && !(pos&(EQP_HEAD_TOP|EQP_HEAD_MID)))
			sd->status.head_bottom = id->look;
		else
			sd->status.head_bottom = 0;
		clif_changelook(&sd->bl,LOOK_HEAD_BOTTOM,sd->status.head_bottom);
	}
	if(pos & EQP_HEAD_TOP) {
		if(id)
			sd->status.head_top = id->look;
		else
			sd->status.head_top = 0;
		clif_changelook(&sd->bl,LOOK_HEAD_TOP,sd->status.head_top);
	}
	if(pos & EQP_HEAD_MID) {
		if(id && !(pos&EQP_HEAD_TOP))
			sd->status.head_mid = id->look;
		else
			sd->status.head_mid = 0;
		clif_changelook(&sd->bl,LOOK_HEAD_MID,sd->status.head_mid);
	}
	if(pos & EQP_SHOES)
		clif_changelook(&sd->bl,LOOK_SHOES,0);

	pc_checkallowskill(sd); //Check if status changes should be halted.


	status_calc_pc(sd,0);
	if (flag) //Update skill data
		clif_skillinfoblock(sd);

	//OnEquip script [Skotlex]
	if (id) {
		int i;
		struct item_data *data;
		if (id->equip_script)
			run_script(id->equip_script,0,sd->bl.id,fake_nd->bl.id);
		if(itemdb_isspecial(sd->status.inventory[n].card[0]))
			; //No cards
		else
		for(i=0;i<id->slot; i++)
		{
			if (!sd->status.inventory[n].card[i])
				continue;
			data = itemdb_exists(sd->status.inventory[n].card[i]);
			if (data && data->equip_script)
				run_script(data->equip_script,0,sd->bl.id,fake_nd->bl.id);
		}
	}
	return 0;
}

/*==========================================
 * ? õµ½¨ðO·
 * type:
 * 0 - only unequip
 * 1 - calculate status after unequipping
 * 2 - force unequip
 *------------------------------------------*/
int pc_unequipitem(struct map_session_data *sd,int n,int flag)
{
	int i;
	nullpo_retr(0, sd);

	if( n < 0 || n >= MAX_INVENTORY ) {
		clif_unequipitemack(sd,0,0,0);
		return 0;
	}

	// if player is berserk then cannot unequip
	if( !(flag&2) && sd->sc.count && sd->sc.data[SC_BERSERK] )
	{
		clif_unequipitemack(sd,n,0,0);
		return 0;
	}

	if(battle_config.battle_log)
		ShowInfo("unequip %d %x:%x\n",n,pc_equippoint(sd,n),sd->status.inventory[n].equip);

	if(!sd->status.inventory[n].equip){ //Nothing to unequip
		clif_unequipitemack(sd,n,0,0);
		return 0;
	}
	for(i=0;i<EQI_MAX;i++) {
		if(sd->status.inventory[n].equip & equip_pos[i])
			sd->equip_index[i] = -1;
	}

	if(sd->status.inventory[n].equip & EQP_HAND_R) {
		sd->weapontype1 = 0;
		sd->status.weapon = sd->weapontype2;
		pc_calcweapontype(sd);
		clif_changelook(&sd->bl,LOOK_WEAPON,sd->status.weapon);
		if(sd->sc.data[SC_DANCING]) //When unequipping, stop dancing. [Skotlex]
			status_change_end(&sd->bl, SC_DANCING, -1);
	}
	if(sd->status.inventory[n].equip & EQP_HAND_L) {
		sd->status.shield = sd->weapontype2 = 0;
		pc_calcweapontype(sd);
		clif_changelook(&sd->bl,LOOK_SHIELD,sd->status.shield);
	}
	if(sd->status.inventory[n].equip & EQP_HEAD_LOW) {
		sd->status.head_bottom = 0;
		clif_changelook(&sd->bl,LOOK_HEAD_BOTTOM,sd->status.head_bottom);
	}
	if(sd->status.inventory[n].equip & EQP_HEAD_TOP) {
		sd->status.head_top = 0;
		clif_changelook(&sd->bl,LOOK_HEAD_TOP,sd->status.head_top);
	}
	if(sd->status.inventory[n].equip & EQP_HEAD_MID) {
		sd->status.head_mid = 0;
		clif_changelook(&sd->bl,LOOK_HEAD_MID,sd->status.head_mid);
	}
	if(sd->status.inventory[n].equip & EQP_SHOES)
		clif_changelook(&sd->bl,LOOK_SHOES,0);

	clif_unequipitemack(sd,n,sd->status.inventory[n].equip,1);

	if((sd->status.inventory[n].equip & EQP_ARMS) && 
		sd->weapontype1 == 0 && sd->weapontype2 == 0 && (!sd->sc.data[SC_SEVENWIND] || sd->sc.data[SC_ASPERSIO])) //Check for seven wind (but not level seven!)
		skill_enchant_elemental_end(&sd->bl,-1);

	if(sd->status.inventory[n].equip & EQP_ARMOR) {
		// On Armor Change...
		if( sd->sc.data[SC_BENEDICTIO] )
			status_change_end(&sd->bl, SC_BENEDICTIO, -1);
		if( sd->sc.data[SC_ARMOR_RESIST] )
			status_change_end(&sd->bl, SC_ARMOR_RESIST, -1);
	}

	if( sd->state.autobonus&sd->status.inventory[n].equip )
		sd->state.autobonus &= ~sd->status.inventory[n].equip; //Check for activated autobonus [Inkfish]

	sd->status.inventory[n].equip=0;

	if(flag&1) {
		pc_checkallowskill(sd);
		status_calc_pc(sd,0);
	}

	if(sd->sc.data[SC_SIGNUMCRUCIS] && !battle_check_undead(sd->battle_status.race,sd->battle_status.def_ele))
		status_change_end(&sd->bl,SC_SIGNUMCRUCIS,-1);

	//OnUnEquip script [Skotlex]
	if (sd->inventory_data[n]) {
		struct item_data *data;
		if (sd->inventory_data[n]->unequip_script)
			run_script(sd->inventory_data[n]->unequip_script,0,sd->bl.id,fake_nd->bl.id);
		if(itemdb_isspecial(sd->status.inventory[n].card[0]))
			; //No cards
		else
		for(i=0;i<sd->inventory_data[n]->slot; i++)
		{
			if (!sd->status.inventory[n].card[i])
				continue;
			data = itemdb_exists(sd->status.inventory[n].card[i]);
			if (data && data->unequip_script)
				run_script(data->unequip_script,0,sd->bl.id,fake_nd->bl.id);
		}
	}

	return 0;
}

/*==========================================
 * ACeÌindexÔ?ðlß½è
 * ? õiÌ?õÂ\`FbNðsÈ¤
 *------------------------------------------*/
int pc_checkitem(struct map_session_data *sd)
{
	int i,j,k,id,calc_flag = 0;
	struct item_data *it=NULL;

	nullpo_retr(0, sd);

	if( sd->vender_id ) //Avoid reorganizing items when we are vending, as that leads to exploits (pointed out by End of Exam)
		return 0;
	
	for( i = j = 0; i < MAX_INVENTORY; i++ )
	{
		if( (id = sd->status.inventory[i].nameid) == 0 )
			continue;

		if( battle_config.item_check && !itemdb_available(id) )
		{
			ShowWarning("illegal item id %d in %d[%s] inventory.\n",id,sd->bl.id,sd->status.name);
			pc_delitem(sd,i,sd->status.inventory[i].amount,3);
			continue;
		}
		if( i > j )
		{
			memcpy(&sd->status.inventory[j], &sd->status.inventory[i], sizeof(struct item));
			sd->inventory_data[j] = sd->inventory_data[i];
		}
		j++;
	}

	if( j < MAX_INVENTORY )
		memset(&sd->status.inventory[j], 0, sizeof(struct item)*(MAX_INVENTORY-j));
	for( k = j ; k < MAX_INVENTORY; k++ )
		sd->inventory_data[k] = NULL;

	for( i = j = 0; i < MAX_CART; i++ )
	{
		if( (id=sd->status.cart[i].nameid) == 0 )
			continue;
		if( battle_config.item_check &&  !itemdb_available(id) ){
			ShowWarning("illegal item id %d in %d[%s] cart.\n",id,sd->bl.id,sd->status.name);
			pc_cart_delitem(sd,i,sd->status.cart[i].amount,1);
			continue;
		}
		if( i > j )
		{
			memcpy(&sd->status.cart[j],&sd->status.cart[i],sizeof(struct item));
		}
		j++;
	}
	if( j < MAX_CART )
		memset(&sd->status.cart[j],0,sizeof(struct item)*(MAX_CART-j));

	for( i = 0; i < MAX_INVENTORY; i++)
	{
		it = sd->inventory_data[i];

		if( sd->status.inventory[i].nameid == 0 )
			continue;

		if( !sd->status.inventory[i].equip )
			continue;

		if( sd->status.inventory[i].equip&~pc_equippoint(sd,i) )
		{
			pc_unequipitem(sd, i, 2);
			calc_flag = 1;
			continue;
		}

		if( it )
		{ // check for forbiden items.
			int flag =
					(map[sd->bl.m].flag.restricted?map[sd->bl.m].zone:0)
					| (map[sd->bl.m].flag.pvp?1:0)
					| (map_flag_gvg(sd->bl.m)?2:0);
			if( flag && (it->flag.no_equip&flag || !pc_isAllowedCardOn(sd,it->slot,i,flag)) )
			{
				pc_unequipitem(sd, i, 2);
				calc_flag = 1;
			}
		}
	}

	pc_setequipindex(sd);
	if( calc_flag && sd->state.active )
	{
		pc_checkallowskill(sd);
		status_calc_pc(sd,0);
	}

	return 0;
}

/*==========================================
 * PVPÊvZp(foreachinarea)
 *------------------------------------------*/
int pc_calc_pvprank_sub(struct block_list *bl,va_list ap)
{
	struct map_session_data *sd1,*sd2=NULL;

	sd1=(struct map_session_data *)bl;
	sd2=va_arg(ap,struct map_session_data *);

	if( sd1->pvp_point > sd2->pvp_point )
		sd2->pvp_rank++;
	return 0;
}
/*==========================================
 * PVPÊvZ
 *------------------------------------------*/
int pc_calc_pvprank(struct map_session_data *sd)
{
	int old;
	struct map_data *m;
	m=&map[sd->bl.m];
	old=sd->pvp_rank;
	sd->pvp_rank=1;
	map_foreachinmap(pc_calc_pvprank_sub,sd->bl.m,BL_PC,sd);
	if(old!=sd->pvp_rank || sd->pvp_lastusers!=m->users)
		clif_pvpset(sd,sd->pvp_rank,sd->pvp_lastusers=m->users,0);
	return sd->pvp_rank;
}
/*==========================================
 * PVPÊvZ(timer)
 *------------------------------------------*/
int pc_calc_pvprank_timer(int tid, unsigned int tick, int id, intptr data)
{
	struct map_session_data *sd=NULL;

	sd=map_id2sd(id);
	if(sd==NULL)
		return 0;
	sd->pvp_timer = INVALID_TIMER;
	if( pc_calc_pvprank(sd) > 0 )
		sd->pvp_timer = add_timer(gettick()+PVP_CALCRANK_INTERVAL,pc_calc_pvprank_timer,id,data);
	return 0;
}

/*==========================================
 * sdÍ¥µÄ¢é©(?¥ÌêÍûÌchar_idðÔ·)
 *------------------------------------------*/
int pc_ismarried(struct map_session_data *sd)
{
	if(sd == NULL)
		return -1;
	if(sd->status.partner_id > 0)
		return sd->status.partner_id;
	else
		return 0;
}
/*==========================================
 * sdªdstsdÆ¥(dstsd¨sdÌ¥?à¯bÉs¤)
 *------------------------------------------*/
int pc_marriage(struct map_session_data *sd,struct map_session_data *dstsd)
{
	if(sd == NULL || dstsd == NULL ||
		sd->status.partner_id > 0 || dstsd->status.partner_id > 0 ||
		sd->class_&JOBL_BABY)
		return -1;
	sd->status.partner_id = dstsd->status.char_id;
	dstsd->status.partner_id = sd->status.char_id;
	return 0;
}

/*==========================================
 * Divorce sd from its partner
 *------------------------------------------*/
int pc_divorce(struct map_session_data *sd)
{
	struct map_session_data *p_sd;
	int i;

	if( sd == NULL || !pc_ismarried(sd) )
		return -1;

	if( !sd->status.partner_id )
		return -1; // Char is not married

	if( (p_sd = map_charid2sd(sd->status.partner_id)) == NULL )
	{ // Lets char server do the divorce
#ifndef TXT_ONLY
		if( chrif_divorce(sd->status.char_id, sd->status.partner_id) )
			return -1; // No char server connected

		return 0;
#else
		ShowError("pc_divorce: p_sd nullpo\n");
		return -1;
#endif
	}

	// Both players online, lets do the divorce manually
	sd->status.partner_id = 0;
	p_sd->status.partner_id = 0;
	for( i = 0; i < MAX_INVENTORY; i++ )
	{
		if( sd->status.inventory[i].nameid == WEDDING_RING_M || sd->status.inventory[i].nameid == WEDDING_RING_F )
			pc_delitem(sd, i, 1, 0);
		if( p_sd->status.inventory[i].nameid == WEDDING_RING_M || p_sd->status.inventory[i].nameid == WEDDING_RING_F )
			pc_delitem(p_sd, i, 1, 0);
	}

	clif_divorced(sd, p_sd->status.name);
	clif_divorced(p_sd, sd->status.name);

	return 0;
}

/*==========================================
 * sdÌûÌmap_session_dataðÔ·
 *------------------------------------------*/
struct map_session_data *pc_get_partner(struct map_session_data *sd)
{
	if (sd && pc_ismarried(sd))
		// charid2sd returns NULL if not found
		return map_charid2sd(sd->status.partner_id);

	return NULL;
}

struct map_session_data *pc_get_father (struct map_session_data *sd)
{
	if (sd && sd->class_&JOBL_BABY && sd->status.father > 0)
		// charid2sd returns NULL if not found
		return map_charid2sd(sd->status.father);

	return NULL;
}

struct map_session_data *pc_get_mother (struct map_session_data *sd)
{
	if (sd && sd->class_&JOBL_BABY && sd->status.mother > 0)
		// charid2sd returns NULL if not found
		return map_charid2sd(sd->status.mother);

	return NULL;
}

struct map_session_data *pc_get_child (struct map_session_data *sd)
{
	if (sd && pc_ismarried(sd) && sd->status.child > 0)
		// charid2sd returns NULL if not found
		return map_charid2sd(sd->status.child);

	return NULL;
}

void pc_bleeding (struct map_session_data *sd, unsigned int diff_tick)
{
	int hp = 0, sp = 0;

	if( pc_isdead(sd) )
		return;

	if (sd->hp_loss.value) {
		sd->hp_loss.tick += diff_tick;
		while (sd->hp_loss.tick >= sd->hp_loss.rate) {
			hp += sd->hp_loss.value;
			sd->hp_loss.tick -= sd->hp_loss.rate;
		}
		if(hp >= sd->battle_status.hp)
			hp = sd->battle_status.hp-1; //Script drains cannot kill you.
	}
	
	if (sd->sp_loss.value) {
		sd->sp_loss.tick += diff_tick;
		while (sd->sp_loss.tick >= sd->sp_loss.rate) {
			sp += sd->sp_loss.value;
			sd->sp_loss.tick -= sd->sp_loss.rate;
		}
	}

	if (hp > 0 || sp > 0)
		status_zap(&sd->bl, hp, sp);

	return;
}

//Character regen. Flag is used to know which types of regen can take place.
//&1: HP regen
//&2: SP regen
void pc_regen (struct map_session_data *sd, unsigned int diff_tick)
{
	int hp = 0, sp = 0;

	if (sd->hp_regen.value) {
		sd->hp_regen.tick += diff_tick;
		while (sd->hp_regen.tick >= sd->hp_regen.rate) {
			hp += sd->hp_regen.value;
			sd->hp_regen.tick -= sd->hp_regen.rate;
		}
	}
	
	if (sd->sp_regen.value) {
		sd->sp_regen.tick += diff_tick;
		while (sd->sp_regen.tick >= sd->sp_regen.rate) {
			sp += sd->sp_regen.value;
			sd->sp_regen.tick -= sd->sp_regen.rate;
		}
	}

	if (hp > 0 || sp > 0)
		status_heal(&sd->bl, hp, sp, 0);

	return;
}

/*==========================================
 * Z?u|CgÌÛ¶
 *------------------------------------------*/
int pc_setsavepoint(struct map_session_data *sd, short mapindex,int x,int y)
{
	nullpo_retr(0, sd);

	sd->status.save_point.map = mapindex;
	sd->status.save_point.x = x;
	sd->status.save_point.y = y;

	return 0;
}

/*==========================================
 * ©®Z?u (timer??)
 *------------------------------------------*/
int pc_autosave(int tid, unsigned int tick, int id, intptr data)
{
	int interval;
	struct s_mapiterator* iter;
	struct map_session_data* sd;
	static int last_save_id = 0, save_flag = 0;

	if(save_flag == 2) //Someone was saved on last call, normal cycle
		save_flag = 0;
	else
		save_flag = 1; //Noone was saved, so save first found char.

	iter = mapit_getallusers();
	for( sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC*)mapit_next(iter) )
	{
		if(sd->bl.id == last_save_id && save_flag != 1) {
			save_flag = 1;
			continue;
		}

		if(save_flag != 1) //Not our turn to save yet.
			continue;

		//Save char.
		last_save_id = sd->bl.id;
		save_flag = 2;

		chrif_save(sd,0);
		break;
	}
	mapit_free(iter);

	interval = autosave_interval/(map_usercount()+1);
	if(interval < minsave_interval)
		interval = minsave_interval;
	add_timer(gettick()+interval,pc_autosave,0,0);

	return 0;
}

static int pc_daynight_timer_sub(struct map_session_data *sd,va_list ap)
{
	if (sd->state.night != night_flag && map[sd->bl.m].flag.nightenabled)
  	{	//Night/day state does not match.
		clif_status_load(&sd->bl, SI_NIGHT, night_flag); //New night effect by dynamix [Skotlex]
		sd->state.night = night_flag;
		return 1;
	}
	return 0;
}
/*================================================
 * timer to do the day [Yor]
 * data: 0 = called by timer, 1 = gmcommand/script
 *------------------------------------------------*/
int map_day_timer(int tid, unsigned int tick, int id, intptr data)
{
	char tmp_soutput[1024];

	if (data == 0 && battle_config.day_duration <= 0)	// if we want a day
		return 0;
	
	if (!night_flag)
		return 0; //Already day.
	
	night_flag = 0; // 0=day, 1=night [Yor]
	map_foreachpc(pc_daynight_timer_sub);
	strcpy(tmp_soutput, (data == 0) ? msg_txt(502) : msg_txt(60)); // The day has arrived!
	intif_broadcast(tmp_soutput, strlen(tmp_soutput) + 1, 0);
	return 0;
}

/*================================================
 * timer to do the night [Yor]
 * data: 0 = called by timer, 1 = gmcommand/script
 *------------------------------------------------*/
int map_night_timer(int tid, unsigned int tick, int id, intptr data)
{
	char tmp_soutput[1024];

	if (data == 0 && battle_config.night_duration <= 0)	// if we want a night
		return 0;
	
	if (night_flag)
		return 0; //Already nigth.

	night_flag = 1; // 0=day, 1=night [Yor]
	map_foreachpc(pc_daynight_timer_sub);
	strcpy(tmp_soutput, (data == 0) ? msg_txt(503) : msg_txt(59)); // The night has fallen...
	intif_broadcast(tmp_soutput, strlen(tmp_soutput) + 1, 0);
	return 0;
}

void pc_setstand(struct map_session_data *sd){
	nullpo_retv(sd);

	if(sd->sc.data[SC_TENSIONRELAX])
		status_change_end(&sd->bl,SC_TENSIONRELAX,-1);

	//Reset sitting tick.
	sd->ssregen.tick.hp = sd->ssregen.tick.sp = 0;
	sd->state.dead_sit = sd->vd.dead_sit = 0;
}

/*==========================================
 * Duel organizing functions [LuzZza]
 *------------------------------------------*/
void duel_savetime(struct map_session_data* sd)
{
	time_t timer;
	struct tm *t;
	
	time(&timer);
	t = localtime(&timer);
	
	pc_setglobalreg(sd, "PC_LAST_DUEL_TIME", t->tm_mday*24*60 + t->tm_hour*60 + t->tm_min);	
	return;
}

int duel_checktime(struct map_session_data* sd)
{
	int diff;
	time_t timer;
	struct tm *t;
	
	time(&timer);
    t = localtime(&timer);
	
	diff = t->tm_mday*24*60 + t->tm_hour*60 + t->tm_min - pc_readglobalreg(sd, "PC_LAST_DUEL_TIME");
	
	return !(diff >= 0 && diff < battle_config.duel_time_interval);
}
static int duel_showinfo_sub(struct map_session_data* sd, va_list va)
{
	struct map_session_data *ssd = va_arg(va, struct map_session_data*);
	int *p = va_arg(va, int*);
	char output[256];

	if (sd->duel_group != ssd->duel_group) return 0;
	
	sprintf(output, "      %d. %s", ++(*p), sd->status.name);
	clif_disp_onlyself(ssd, output, strlen(output));
	return 1;
}

int duel_showinfo(const unsigned int did, struct map_session_data* sd)
{
	int p=0;
	char output[256];

	if(duel_list[did].max_players_limit > 0)
		sprintf(output, msg_txt(370), //" -- Duels: %d/%d, Members: %d/%d, Max players: %d --"
			did, duel_count,
			duel_list[did].members_count,
			duel_list[did].members_count + duel_list[did].invites_count,
			duel_list[did].max_players_limit);
	else
		sprintf(output, msg_txt(371), //" -- Duels: %d/%d, Members: %d/%d --"
			did, duel_count,
			duel_list[did].members_count,
			duel_list[did].members_count + duel_list[did].invites_count);

	clif_disp_onlyself(sd, output, strlen(output));
	map_foreachpc(duel_showinfo_sub, sd, &p);
	return 0;
}

int duel_create(struct map_session_data* sd, const unsigned int maxpl)
{
	int i=1;
	char output[256];
	
	while(duel_list[i].members_count > 0 && i < MAX_DUEL) i++;
	if(i == MAX_DUEL) return 0;
	
	duel_count++;
	sd->duel_group = i;
	duel_list[i].members_count++;
	duel_list[i].invites_count = 0;
	duel_list[i].max_players_limit = maxpl;
	
	strcpy(output, msg_txt(372)); // " -- Duel has been created (@invite/@leave) --"
	clif_disp_onlyself(sd, output, strlen(output));
	
	clif_set0199(sd, 1);
	//clif_misceffect2(&sd->bl, 159);
	return i;
}

int duel_invite(const unsigned int did, struct map_session_data* sd, struct map_session_data* target_sd)
{
	char output[256];

	// " -- Player %s invites %s to duel --"
	sprintf(output, msg_txt(373), sd->status.name, target_sd->status.name);
	clif_disp_message(&sd->bl, output, strlen(output), DUEL_WOS);

	target_sd->duel_invite = did;
	duel_list[did].invites_count++;
	
	// "Blue -- Player %s invites you to PVP duel (@accept/@reject) --"
	sprintf(output, msg_txt(374), sd->status.name);
	clif_broadcast((struct block_list *)target_sd, output, strlen(output)+1, 0x10, SELF);
	return 0;
}

static int duel_leave_sub(struct map_session_data* sd, va_list va)
{
	int did = va_arg(va, int);
	if (sd->duel_invite == did)
		sd->duel_invite = 0;
	return 0;
}

int duel_leave(const unsigned int did, struct map_session_data* sd)
{
	char output[256];
	
	// " <- Player %s has left duel --"
	sprintf(output, msg_txt(375), sd->status.name);
	clif_disp_message(&sd->bl, output, strlen(output), DUEL_WOS);
	
	duel_list[did].members_count--;
	
	if(duel_list[did].members_count == 0) {
		map_foreachpc(duel_leave_sub, did); 
		duel_count--;
	}
	
	sd->duel_group = 0;
	duel_savetime(sd);
	clif_set0199(sd, 0);
	return 0;
}

int duel_accept(const unsigned int did, struct map_session_data* sd)
{
	char output[256];
	
	duel_list[did].members_count++;
	sd->duel_group = sd->duel_invite;
	duel_list[did].invites_count--;
	sd->duel_invite = 0;
	
	// " -> Player %s has accepted duel --"
	sprintf(output, msg_txt(376), sd->status.name);
	clif_disp_message(&sd->bl, output, strlen(output), DUEL_WOS);

	clif_set0199(sd, 1);
	//clif_misceffect2(&sd->bl, 159);
	return 0;
}

int duel_reject(const unsigned int did, struct map_session_data* sd)
{
	char output[256];
	
	// " -- Player %s has rejected duel --"
	sprintf(output, msg_txt(377), sd->status.name);
	clif_disp_message(&sd->bl, output, strlen(output), DUEL_WOS);
	
	duel_list[did].invites_count--;
	sd->duel_invite = 0;
	return 0;
}

int pc_split_str(char *str,char **val,int num)
{
	int i;

	for (i=0; i<num && str; i++){
		val[i] = str;
		str = strchr(str,',');
		if (str && i<num-1) //Do not remove a trailing comma.
			*str++=0;
	}
	return i;
}

int pc_split_atoi(char* str, int* val, char sep, int max)
{
	int i,j;
	for (i=0; i<max; i++) {
		if (!str) break;
		val[i] = atoi(str);
		str = strchr(str,sep);
		if (str)
			*str++=0;
	}
	//Zero up the remaining.
	for(j=i; j < max; j++)
		val[j] = 0;
	return i;
}

int pc_split_atoui(char* str, unsigned int* val, char sep, int max)
{
	static int warning=0;
	int i,j;
	double f;
	for (i=0; i<max; i++) {
		if (!str) break;
		f = atof(str);
		if (f < 0)
			val[i] = 0;
		else if (f > UINT_MAX) {
			val[i] = UINT_MAX;
			if (!warning) {
				warning = 1;
				ShowWarning("pc_readdb (exp.txt): Required exp per level is capped to %u\n", UINT_MAX);
			}
		} else
			val[i] = (unsigned int)f;
		str = strchr(str,sep);
		if (str)
			*str++=0;
	}
	//Zero up the remaining.
	for(j=i; j < max; j++)
		val[j] = 0;
	return i;
}

/*==========================================
 * DB reading.
 * exp.txt        - required experience values
 * job_db1.txt    - weight, hp, sp, aspd
 * job_db2.txt    - job level stat bonuses
 * skill_tree.txt - skill tree for every class
 * attr_fix.txt   - elemental adjustment table
 * size_fix.txt   - size adjustment table for weapons
 * refine_db.txt  - refining data table
 *------------------------------------------*/
int pc_readdb(void)
{
	int i,j,k;
	unsigned int stat;
	FILE *fp;
	char line[24000],*p;

	// Kv??l?Ý?Ý
	memset(exp_table,0,sizeof(exp_table));
	memset(max_level,0,sizeof(max_level));
	sprintf(line, "%s/exp.txt", db_path);
	fp=fopen(line, "r");
	if(fp==NULL){
		ShowError("can't read %s\n", line);
		return 1;
	}
	while(fgets(line, sizeof(line), fp))
	{
		int jobs[CLASS_COUNT], job_count, job, job_id;
		int type;
		unsigned int ui,maxlv;
		char *split[4];
		if(line[0]=='/' && line[1]=='/')
			continue;
		if (pc_split_str(line,split,4) < 4)
			continue;
		
		job_count = pc_split_atoi(split[1],jobs,':',CLASS_COUNT);
		if (job_count < 1)
			continue;
		job_id = jobs[0];
		if (!pcdb_checkid(job_id)) {
			ShowError("pc_readdb: Invalid job ID %d.\n", job_id);
			continue;
		}
		type = atoi(split[2]);
		if (type < 0 || type > 1) {
			ShowError("pc_readdb: Invalid type %d (must be 0 for base levels, 1 for job levels).\n", type);
			continue;
		}
		maxlv = atoi(split[0]);
		if (maxlv > MAX_LEVEL) {
			ShowWarning("pc_readdb: Specified max level %u for job %d is beyond server's limit (%u).\n ", maxlv, job_id, MAX_LEVEL);
			maxlv = MAX_LEVEL;
		}
		
		job = jobs[0] = pc_class2idx(job_id);
		//We send one less and then one more because the last entry in the exp array should hold 0.
		max_level[job][type] = pc_split_atoui(split[3], exp_table[job][type],',',maxlv-1)+1;
		//Reverse check in case the array has a bunch of trailing zeros... [Skotlex]
		//The reasoning behind the -2 is this... if the max level is 5, then the array
		//should look like this:
	   //0: x, 1: x, 2: x: 3: x 4: 0 <- last valid value is at 3.
		while ((ui = max_level[job][type]) >= 2 && exp_table[job][type][ui-2] <= 0)
			max_level[job][type]--;
		if (max_level[job][type] < maxlv) {
			ShowWarning("pc_readdb: Specified max %u for job %d, but that job's exp table only goes up to level %u.\n", maxlv, job_id, max_level[job][type]);
			ShowInfo("Filling the missing values with the last exp entry.\n");
			//Fill the requested values with the last entry.
			ui = (max_level[job][type] <= 2? 0: max_level[job][type]-2);
			for (; ui+2 < maxlv; ui++)
				exp_table[job][type][ui] = exp_table[job][type][ui-1];
			max_level[job][type] = maxlv;
		}
//		ShowDebug("%s - Class %d: %d\n", type?"Job":"Base", job_id, max_level[job][type]);
		for (i = 1; i < job_count; i++) {
			job_id = jobs[i];
			if (!pcdb_checkid(job_id)) {
				ShowError("pc_readdb: Invalid job ID %d.\n", job_id);
				continue;
			}
			job = pc_class2idx(job_id);
			memcpy(exp_table[job][type], exp_table[jobs[0]][type], sizeof(exp_table[0][0]));
			max_level[job][type] = maxlv;
//			ShowDebug("%s - Class %d: %u\n", type?"Job":"Base", job_id, max_level[job][type]);
		}
	}
	fclose(fp);
	for (i = 0; i < JOB_MAX; i++) {
		if (!pcdb_checkid(i)) continue;
		if (i == JOB_WEDDING || i == JOB_XMAS || i == JOB_SUMMER)
			continue; //Classes that do not need exp tables.
		j = pc_class2idx(i);
		if (!max_level[j][0])
			ShowWarning("Class %s (%d) does not has a base exp table.\n", job_name(i), i);
		if (!max_level[j][1])
			ShowWarning("Class %s (%d) does not has a job exp table.\n", job_name(i), i);
	}
	ShowStatus("Done reading '"CL_WHITE"%s"CL_RESET"'.\n","exp.txt");

	// XLc?
	memset(skill_tree,0,sizeof(skill_tree));
	sprintf(line, "%s/skill_tree.txt", db_path);
	fp=fopen(line,"r");
	if(fp==NULL){
		ShowError("can't read %s\n", line);
		return 1;
	}

	while(fgets(line, sizeof(line), fp))
	{
		char *split[50];
		int f=0, m=3, idx;
		if(line[0]=='/' && line[1]=='/')
			continue;
		for(j=0,p=line;j<14 && p;j++){
			split[j]=p;
			p=strchr(p,',');
			if(p) *p++=0;
		}
		if(j<13)
			continue;
		if (j == 14) {
			f=1;	// MinJobLvl has been added
			m++;
		}
		// check for bounds [celest]
		idx = atoi(split[0]);
		if(!pcdb_checkid(idx))
			continue;
		idx = pc_class2idx(idx);
		k = atoi(split[1]); //This is to avoid adding two lines for the same skill. [Skotlex]
		ARR_FIND( 0, MAX_SKILL_TREE, j, skill_tree[idx][j].id == 0 || skill_tree[idx][j].id == k );
		if( j == MAX_SKILL_TREE )
		{
			ShowWarning("Unable to load skill %d into job %d's tree. Maximum number of skills per class has been reached.\n", k, atoi(split[0]));
			continue;
		}
		skill_tree[idx][j].id=k;
		skill_tree[idx][j].max=atoi(split[2]);
		if (f) skill_tree[idx][j].joblv=atoi(split[3]);

		for(k=0;k<5;k++){
			skill_tree[idx][j].need[k].id=atoi(split[k*2+m]);
			skill_tree[idx][j].need[k].lv=atoi(split[k*2+m+1]);
		}
	}
	fclose(fp);
	ShowStatus("Done reading '"CL_WHITE"%s"CL_RESET"'.\n","skill_tree.txt");

	// ?«C³e?u
	for(i=0;i<4;i++)
		for(j=0;j<ELE_MAX;j++)
			for(k=0;k<ELE_MAX;k++)
				attr_fix_table[i][j][k]=100;

	sprintf(line, "%s/attr_fix.txt", db_path);
	fp=fopen(line,"r");
	if(fp==NULL){
		ShowError("can't read %s\n", line);
		return 1;
	}
	while(fgets(line, sizeof(line), fp))
	{
		char *split[10];
		int lv,n;
		if(line[0]=='/' && line[1]=='/')
			continue;
		for(j=0,p=line;j<3 && p;j++){
			split[j]=p;
			p=strchr(p,',');
			if(p) *p++=0;
		}
		if( j < 2 )
			continue;

		lv=atoi(split[0]);
		n=atoi(split[1]);

		for(i=0;i<n && i<ELE_MAX;){
			if( !fgets(line, sizeof(line), fp) )
				break;
			if(line[0]=='/' && line[1]=='/')
				continue;

			for(j=0,p=line;j<n && j<ELE_MAX && p;j++){
				while(*p==32 && *p>0)
					p++;
				attr_fix_table[lv-1][i][j]=atoi(p);
				if(battle_config.attr_recover == 0 && attr_fix_table[lv-1][i][j] < 0)
					attr_fix_table[lv-1][i][j] = 0;
				p=strchr(p,',');
				if(p) *p++=0;
			}

			i++;
		}
	}
	fclose(fp);
	ShowStatus("Done reading '"CL_WHITE"%s"CL_RESET"'.\n","attr_fix.txt");

	// XLc?
	memset(statp,0,sizeof(statp));
	i=1;
	stat = 45;	// base points
	sprintf(line, "%s/statpoint.txt", db_path);
	fp=fopen(line,"r");
	if(fp == NULL){
		ShowStatus("Can't read '"CL_WHITE"%s"CL_RESET"'... Generating DB.\n",line);
		//return 1;
	} else {
		while(fgets(line, sizeof(line), fp))
		{
			if(line[0]=='/' && line[1]=='/')
				continue;
			if ((stat=strtoul(line,NULL,10))<0)
				stat=0;
			if (i > MAX_LEVEL)
				break;
			statp[i]=stat;			
			i++;
		}
		fclose(fp);
		ShowStatus("Done reading '"CL_WHITE"%s"CL_RESET"'.\n","statpoint.txt");
	}
	// generate the remaining parts of the db if necessary
	for (; i <= MAX_LEVEL; i++) {
		stat += (i+15)/5;
		statp[i] = stat;		
	}

	return 0;
}

// Read MOTD on startup. [Valaris]
int pc_read_motd(void)
{
	FILE *fp;
	int ln=0,i=0;

	memset(motd_text,0,sizeof(motd_text));
	if ((fp = fopen(motd_txt, "r")) != NULL) {
		while ((ln < MOTD_LINE_SIZE) && fgets(motd_text[ln], sizeof(motd_text[ln])-1, fp) != NULL) {
			if(motd_text[ln][0] == '/' && motd_text[ln][1] == '/')
				continue;
			for(i=0; motd_text[ln][i]; i++) {
				if (motd_text[ln][i] == '\r' || motd_text[ln][i]== '\n') {
					if(i)
						motd_text[ln][i]=0;
					else
						motd_text[ln][0]=' ';
					ln++;
					break;
				}
			}
		}
		fclose(fp);
	}
	else
		ShowWarning("In function pc_read_motd() -> File '"CL_WHITE"%s"CL_RESET"' not found.\n", motd_txt);
	
	return 0;
}

/*==========================================
 * pc? Wú»
 *------------------------------------------*/
void do_final_pc(void)
{
	return;
}

int do_init_pc(void)
{
	pc_readdb();
	pc_read_motd(); // Read MOTD [Valaris]

	memset(&duel_list[0], 0, sizeof(duel_list));

	add_timer_func_list(pc_invincible_timer, "pc_invincible_timer");
	add_timer_func_list(pc_eventtimer, "pc_eventtimer");
	add_timer_func_list(pc_inventory_rental_end, "pc_inventory_rental_end");
	add_timer_func_list(pc_calc_pvprank_timer, "pc_calc_pvprank_timer");
	add_timer_func_list(pc_autosave, "pc_autosave");
	add_timer_func_list(pc_spiritball_timer, "pc_spiritball_timer");
	add_timer_func_list(pc_follow_timer, "pc_follow_timer");
	add_timer_func_list(pc_endautobonus, "pc_endautobonus");

	add_timer(gettick() + autosave_interval, pc_autosave, 0, 0);

	if (battle_config.day_duration > 0 && battle_config.night_duration > 0) {
		int day_duration = battle_config.day_duration;
		int night_duration = battle_config.night_duration;
		// add night/day timer (by [yor])
		add_timer_func_list(map_day_timer, "map_day_timer"); // by [yor]
		add_timer_func_list(map_night_timer, "map_night_timer"); // by [yor]

		if (!battle_config.night_at_start) {
			night_flag = 0; // 0=day, 1=night [Yor]
			day_timer_tid = add_timer_interval(gettick() + day_duration + night_duration, map_day_timer, 0, 0, day_duration + night_duration);
			night_timer_tid = add_timer_interval(gettick() + day_duration, map_night_timer, 0, 0, day_duration + night_duration);
		} else {
			night_flag = 1; // 0=day, 1=night [Yor]
			day_timer_tid = add_timer_interval(gettick() + night_duration, map_day_timer, 0, 0, day_duration + night_duration);
			night_timer_tid = add_timer_interval(gettick() + day_duration + night_duration, map_night_timer, 0, 0, day_duration + night_duration);
		}
	}

	return 0;
}
