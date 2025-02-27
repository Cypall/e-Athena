// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/cbasetypes.h"
#include "../common/core.h"
#include "../common/timer.h"
#include "../common/grfio.h"
#include "../common/malloc.h"
#include "../common/socket.h" // WFIFO*()
#include "../common/showmsg.h"
#include "../common/version.h"
#include "../common/nullpo.h"
#include "../common/strlib.h"
#include "../common/utils.h"

#include "map.h"
#include "path.h"
#include "chrif.h"
#include "clif.h"
#include "intif.h"
#include "npc.h"
#include "pc.h"
#include "status.h"
#include "mob.h"
#include "npc.h" // npc_setcells(), npc_unsetcells()
#include "chat.h"
#include "itemdb.h"
#include "storage.h"
#include "skill.h"
#include "trade.h"
#include "party.h"
#include "unit.h"
#include "battle.h"
#include "battleground.h"
#include "quest.h"
#include "script.h"
#include "mapreg.h"
#include "guild.h"
#include "pet.h"
#include "homunculus.h"
#include "instance.h"
#include "mercenary.h"
#include "atcommand.h"
#include "log.h"
#ifndef TXT_ONLY
#include "mail.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifndef TXT_ONLY
char default_codepage[32] = "";

int map_server_port = 3306;
char map_server_ip[32] = "127.0.0.1";
char map_server_id[32] = "ragnarok";
char map_server_pw[32] = "ragnarok";
char map_server_db[32] = "ragnarok";
Sql* mmysql_handle;

int db_use_sqldbs = 0;
char item_db_db[32] = "item_db";
char item_db2_db[32] = "item_db2";
char mob_db_db[32] = "mob_db";
char mob_db2_db[32] = "mob_db2";

// log database
char log_db_ip[32] = "127.0.0.1";
int log_db_port = 3306;
char log_db_id[32] = "ragnarok";
char log_db_pw[32] = "ragnarok";
char log_db_db[32] = "log";
Sql* logmysql_handle;

#endif /* not TXT_ONLY */

// This param using for sending mainchat
// messages like whispers to this nick. [LuzZza]
char main_chat_nick[16] = "Main";

char *INTER_CONF_NAME;
char *LOG_CONF_NAME;
char *MAP_CONF_NAME;
char *BATTLE_CONF_FILENAME;
char *ATCOMMAND_CONF_FILENAME;
char *CHARCOMMAND_CONF_FILENAME;
char *SCRIPT_CONF_NAME;
char *MSG_CONF_NAME;
char *GRF_PATH_FILENAME;

// 極力 staticでロ?カルに?める
static DBMap* id_db=NULL; // int id -> struct block_list*
static DBMap* pc_db=NULL; // int id -> struct map_session_data*
static DBMap* mobid_db=NULL; // int id -> struct mob_data*
static DBMap* bossid_db=NULL; // int id -> struct mob_data* (MVP db)
static DBMap* map_db=NULL; // unsigned int mapindex -> struct map_data*
static DBMap* nick_db=NULL; // int char_id -> struct charid2nick* (requested names of offline characters)
static DBMap* charid_db=NULL; // int char_id -> struct map_session_data*
static DBMap* regen_db=NULL; // int id -> struct block_list* (status_natural_heal processing)

static int map_users=0;

#define block_free_max 1048576
struct block_list *block_free[block_free_max];
static int block_free_count = 0, block_free_lock = 0;

#define BL_LIST_MAX 1048576
static struct block_list *bl_list[BL_LIST_MAX];
static int bl_list_count = 0;

struct map_data map[MAX_MAP_PER_SERVER];
int map_num = 0;
int map_port=0;

int autosave_interval = DEFAULT_AUTOSAVE_INTERVAL;
int minsave_interval = 100;
int save_settings = 0xFFFF;
int agit_flag = 0;
int agit2_flag = 0;
int night_flag = 0; // 0=day, 1=night [Yor]

struct charid_request {
	struct charid_request* next;
	int charid;// who want to be notified of the nick
};
struct charid2nick {
	char nick[NAME_LENGTH];
	struct charid_request* requests;// requests of notification on this nick
};

// This is the main header found at the very beginning of the map cache
struct map_cache_main_header {
	uint32 file_size;
	uint16 map_count;
};

// This is the header appended before every compressed map cells info in the map cache
struct map_cache_map_info {
	char name[MAP_NAME_LENGTH];
	int16 xs;
	int16 ys;
	int32 len;
};

char map_cache_file[256]="db/map_cache.dat";
char db_path[256] = "db";
char motd_txt[256] = "conf/motd.txt";
char help_txt[256] = "conf/help.txt";
char help2_txt[256] = "conf/help2.txt";
char charhelp_txt[256] = "conf/charhelp.txt";

char wisp_server_name[NAME_LENGTH] = "Server"; // can be modified in char-server configuration file

int console = 0;
int enable_spy = 0; //To enable/disable @spy commands, which consume too much cpu time when sending packets. [Skotlex]
int enable_grf = 0;	//To enable/disable reading maps from GRF files, bypassing mapcache [blackhole89]

/*==========================================
 * server player count (of all mapservers)
 *------------------------------------------*/
void map_setusers(int users)
{
	map_users = users;
}

int map_getusers(void)
{
	return map_users;
}

/*==========================================
 * server player count (this mapserver only)
 *------------------------------------------*/
int map_usercount(void)
{
	return pc_db->size(pc_db);
}

//
// block削除の安全性確保?理
//

/*==========================================
 * blockをfreeするときfreeの?わりに呼ぶ
 * ロックされているときはバッファにためる
 *------------------------------------------*/
int map_freeblock (struct block_list *bl)
{
	nullpo_retr(block_free_lock, bl);
	if (block_free_lock == 0 || block_free_count >= block_free_max)
	{
		aFree(bl);
		bl = NULL;
		if (block_free_count >= block_free_max)
			ShowWarning("map_freeblock: too many free block! %d %d\n", block_free_count, block_free_lock);
	} else
		block_free[block_free_count++] = bl;

	return block_free_lock;
}
/*==========================================
 * blockのfreeを一市Iに禁止する
 *------------------------------------------*/
int map_freeblock_lock (void)
{
	return ++block_free_lock;
}

/*==========================================
 * blockのfreeのロックを解除する
 * このとき、ロックが完全になくなると
 * バッファにたまっていたblockを全部削除
 *------------------------------------------*/
int map_freeblock_unlock (void)
{
	if ((--block_free_lock) == 0) {
		int i;
		for (i = 0; i < block_free_count; i++)
		{
			aFree(block_free[i]);
			block_free[i] = NULL;
		}
		block_free_count = 0;
	} else if (block_free_lock < 0) {
		ShowError("map_freeblock_unlock: lock count < 0 !\n");
		block_free_lock = 0;
	}

	return block_free_lock;
}

// map_freeblock_lock() を呼んで map_freeblock_unlock() を呼ばない
// 関数があったので、定期的にblock_free_lockをリセットするようにする。
// この関数は、do_timer() のトップレベルから呼ばれるので、
// block_free_lock を直接いじっても支障無いはず。

int map_freeblock_timer(int tid, unsigned int tick, int id, intptr data)
{
	if (block_free_lock > 0) {
		ShowError("map_freeblock_timer: block_free_lock(%d) is invalid.\n", block_free_lock);
		block_free_lock = 1;
		map_freeblock_unlock();
	}

	return 0;
}

//
// block化?理
//
/*==========================================
 * map[]のblock_listから?がっている場合に
 * bl->prevにbl_headのアドレスを入れておく
 *------------------------------------------*/
static struct block_list bl_head;

#ifdef CELL_NOSTACK
/*==========================================
 * These pair of functions update the counter of how many objects
 * lie on a tile.
 *------------------------------------------*/
void map_addblcell(struct block_list *bl)
{
	if( bl->m<0 || bl->x<0 || bl->x>=map[bl->m].xs || bl->y<0 || bl->y>=map[bl->m].ys || !(bl->type&BL_CHAR) )
		return;
	map[bl->m].cell[bl->x+bl->y*map[bl->m].xs].cell_bl++;
	return;
}

void map_delblcell(struct block_list *bl)
{
	if( bl->m <0 || bl->x<0 || bl->x>=map[bl->m].xs || bl->y<0 || bl->y>=map[bl->m].ys || !(bl->type&BL_CHAR) )
		return;
	map[bl->m].cell[bl->x+bl->y*map[bl->m].xs].cell_bl--;
}
#endif

/*==========================================
 * Adds a block to the map.
 * Returns 0 on success, 1 on failure (illegal coordinates).
 *------------------------------------------*/
int map_addblock(struct block_list* bl)
{
	int m, x, y, pos;

	nullpo_retr(0, bl);

	if (bl->prev != NULL) {
		ShowError("map_addblock: bl->prev != NULL\n");
		return 1;
	}

	m = bl->m;
	x = bl->x;
	y = bl->y;
	if( m < 0 || m >= map_num )
	{
		ShowError("map_addblock: invalid map id (%d), only %d are loaded.\n", m, map_num);
		return 1;
	}
	if( x < 0 || x >= map[m].xs || y < 0 || y >= map[m].ys )
	{
		ShowError("map_addblock: out-of-bounds coordinates (\"%s\",%d,%d), map is %dx%d\n", map[m].name, x, y, map[m].xs, map[m].ys);
		return 1;
	}

	pos = x/BLOCK_SIZE+(y/BLOCK_SIZE)*map[m].bxs;

	if (bl->type == BL_MOB) {
		bl->next = map[m].block_mob[pos];
		bl->prev = &bl_head;
		if (bl->next) bl->next->prev = bl;
		map[m].block_mob[pos] = bl;
	} else {
		bl->next = map[m].block[pos];
		bl->prev = &bl_head;
		if (bl->next) bl->next->prev = bl;
		map[m].block[pos] = bl;
	}

#ifdef CELL_NOSTACK
	map_addblcell(bl);
#endif
	
	return 0;
}

/*==========================================
 * Removes a block from the map.
 *------------------------------------------*/
int map_delblock(struct block_list* bl)
{
	int pos;
	nullpo_retr(0, bl);

	// ?にblocklistから?けている
	if (bl->prev == NULL) {
		if (bl->next != NULL) {
			// prevがNULLでnextがNULLでないのは有ってはならない
			ShowError("map_delblock error : bl->next!=NULL\n");
		}
		return 0;
	}

#ifdef CELL_NOSTACK
	map_delblcell(bl);
#endif
	
	pos = bl->x/BLOCK_SIZE+(bl->y/BLOCK_SIZE)*map[bl->m].bxs;

	if (bl->next)
		bl->next->prev = bl->prev;
	if (bl->prev == &bl_head) {
		// リストの頭なので、map[]のblock_listを更新する
		if (bl->type == BL_MOB) {
			map[bl->m].block_mob[pos] = bl->next;
		} else {
			map[bl->m].block[pos] = bl->next;
		}
	} else {
		bl->prev->next = bl->next;
	}
	bl->next = NULL;
	bl->prev = NULL;

	return 0;
}

/*==========================================
 * Moves a block a x/y target position. [Skotlex]
 * Pass flag as 1 to prevent doing skill_unit_move checks
 * (which are executed by default on BL_CHAR types)
 *------------------------------------------*/
int map_moveblock(struct block_list *bl, int x1, int y1, unsigned int tick)
{
	int x0 = bl->x, y0 = bl->y;
	struct status_change *sc = NULL;
	int moveblock = ( x0/BLOCK_SIZE != x1/BLOCK_SIZE || y0/BLOCK_SIZE != y1/BLOCK_SIZE);

	if (!bl->prev) {
		//Block not in map, just update coordinates, but do naught else.
		bl->x = x1;
		bl->y = y1;
		return 0;	
	}

	//TODO: Perhaps some outs of bounds checking should be placed here?
	if (bl->type&BL_CHAR) {
		skill_unit_move(bl,tick,2);
		sc = status_get_sc(bl);
		if (sc && sc->count) {
			if (sc->data[SC_CLOSECONFINE])
				status_change_end(bl, SC_CLOSECONFINE, -1);
			if (sc->data[SC_CLOSECONFINE2])
				status_change_end(bl, SC_CLOSECONFINE2, -1);
//			if (sc->data[SC_BLADESTOP]) //Won't stop when you are knocked away, go figure...
//				status_change_end(bl, SC_BLADESTOP, -1);
			if (sc->data[SC_TATAMIGAESHI])
				status_change_end(bl, SC_TATAMIGAESHI, -1);
			if (sc->data[SC_MAGICROD])
				status_change_end(bl, SC_MAGICROD, -1);
		}
	} else
	if (bl->type == BL_NPC)
		npc_unsetcells((TBL_NPC*)bl);

	if (moveblock) map_delblock(bl);
#ifdef CELL_NOSTACK
	else map_delblcell(bl);
#endif
	bl->x = x1;
	bl->y = y1;
	if (moveblock) map_addblock(bl);
#ifdef CELL_NOSTACK
	else map_addblcell(bl);
#endif

	if (bl->type&BL_CHAR) {
		skill_unit_move(bl,tick,3);
		if (sc) {
			if (sc->count) {
				if (sc->data[SC_CLOAKING])
					skill_check_cloaking(bl, sc->data[SC_CLOAKING]);
				if (sc->data[SC_DANCING])
					skill_unit_move_unit_group(skill_id2group(sc->data[SC_DANCING]->val2), bl->m, x1-x0, y1-y0);
				if (sc->data[SC_WARM])
					skill_unit_move_unit_group(skill_id2group(sc->data[SC_WARM]->val4), bl->m, x1-x0, y1-y0);
			}
		}
	} else
	if (bl->type == BL_NPC)
		npc_setcells((TBL_NPC*)bl);

	return 0;
}
	
/*==========================================
 * Counts specified number of objects on given cell.
 *------------------------------------------*/
int map_count_oncell(int m, int x, int y, int type)
{
	int bx,by;
	struct block_list *bl;
	int count = 0;

	if (x < 0 || y < 0 || (x >= map[m].xs) || (y >= map[m].ys))
		return 0;

	bx = x/BLOCK_SIZE;
	by = y/BLOCK_SIZE;

	if (type&~BL_MOB)
		for( bl = map[m].block[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
			if(bl->x == x && bl->y == y && bl->type&type)
				count++;
	
	if (type&BL_MOB)
		for( bl = map[m].block_mob[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
			if(bl->x == x && bl->y == y)
				count++;

	return count;
}
/*
 * ｫｻｫ・ｾｪﾎ�ﾌ��ｪﾋﾌｸｪﾄｪｱｪｿｫｹｫｭｫ・讚ﾋｫﾃｫﾈｪ�ﾚ�ｪ?
 */
struct skill_unit* map_find_skill_unit_oncell(struct block_list* target,int x,int y,int skill_id,struct skill_unit* out_unit)
{
	int m,bx,by;
	struct block_list *bl;
	struct skill_unit *unit;
	m = target->m;

	if (x < 0 || y < 0 || (x >= map[m].xs) || (y >= map[m].ys))
		return NULL;

	bx = x/BLOCK_SIZE;
	by = y/BLOCK_SIZE;

	for( bl = map[m].block[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
	{
		if (bl->x != x || bl->y != y || bl->type != BL_SKILL)
			continue;

		unit = (struct skill_unit *) bl;
		if( unit == out_unit || !unit->alive || !unit->group || unit->group->skill_id != skill_id )
			continue;
		if( battle_check_target(&unit->bl,target,unit->group->target_flag) > 0 )
			return unit;
	}
	return NULL;
}

/*==========================================
 * Adapted from foreachinarea for an easier invocation. [Skotlex]
 *------------------------------------------*/
int map_foreachinrange(int (*func)(struct block_list*,va_list), struct block_list* center, int range, int type, ...)
{
	int bx,by,m;
	int returnCount =0;	//total sum of returned values of func() [Skotlex]
	struct block_list *bl;
	int blockcount=bl_list_count,i;
	int x0,x1,y0,y1;

	m = center->m;
	x0 = max(center->x-range, 0);
	y0 = max(center->y-range, 0);
	x1 = min(center->x+range, map[m].xs-1);
	y1 = min(center->y+range, map[m].ys-1);
	
	if (type&~BL_MOB)
		for (by = y0 / BLOCK_SIZE; by <= y1 / BLOCK_SIZE; by++) {
			for(bx = x0 / BLOCK_SIZE; bx <= x1 / BLOCK_SIZE; bx++) {
				for( bl = map[m].block[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
				{
					if( bl->type&type
						&& bl->x>=x0 && bl->x<=x1 && bl->y>=y0 && bl->y<=y1
#ifdef CIRCULAR_AREA
						&& check_distance_bl(center, bl, range)
#endif
					  	&& bl_list_count<BL_LIST_MAX)
						bl_list[bl_list_count++]=bl;
				}
			}
		}
	if(type&BL_MOB)
		for(by=y0/BLOCK_SIZE;by<=y1/BLOCK_SIZE;by++){
			for(bx=x0/BLOCK_SIZE;bx<=x1/BLOCK_SIZE;bx++){
				for( bl = map[m].block_mob[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
				{
					if( bl->x>=x0 && bl->x<=x1 && bl->y>=y0 && bl->y<=y1
#ifdef CIRCULAR_AREA
						&& check_distance_bl(center, bl, range)
#endif
						&& bl_list_count<BL_LIST_MAX)
						bl_list[bl_list_count++]=bl;
				}
			}
		}

	if(bl_list_count>=BL_LIST_MAX)
		ShowWarning("map_foreachinrange: block count too many!\n");

	map_freeblock_lock();	// メモリからの解放を禁止する

	for(i=blockcount;i<bl_list_count;i++)
		if(bl_list[i]->prev)	// 有?かどうかチェック
		{
			va_list ap;
			va_start(ap, type);
			returnCount += func(bl_list[i], ap);
			va_end(ap);
		}

	map_freeblock_unlock();	// 解放を許可する

	bl_list_count = blockcount;
	return returnCount;	//[Skotlex]
}

/*==========================================
 * Same as foreachinrange, but there must be a shoot-able range between center and target to be counted in. [Skotlex]
 *------------------------------------------*/
int map_foreachinshootrange(int (*func)(struct block_list*,va_list),struct block_list* center, int range, int type,...)
{
	int bx,by,m;
	int returnCount =0;	//total sum of returned values of func() [Skotlex]
	struct block_list *bl;
	int blockcount=bl_list_count,i;
	int x0,x1,y0,y1;

	m = center->m;
	if (m < 0)
		return 0;

	x0 = max(center->x-range, 0);
	y0 = max(center->y-range, 0);
	x1 = min(center->x+range, map[m].xs-1);
	y1 = min(center->y+range, map[m].ys-1);

	if (type&~BL_MOB)
		for(by = y0 / BLOCK_SIZE; by <= y1 / BLOCK_SIZE; by++) {
			for(bx = x0 / BLOCK_SIZE; bx <= x1 / BLOCK_SIZE; bx++) {
				for( bl = map[m].block[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
				{
					if( bl->type&type
						&& bl->x>=x0 && bl->x<=x1 && bl->y>=y0 && bl->y<=y1
#ifdef CIRCULAR_AREA
						&& check_distance_bl(center, bl, range)
#endif
						&& path_search_long(NULL,center->m,center->x,center->y,bl->x,bl->y,CELL_CHKWALL)
					  	&& bl_list_count<BL_LIST_MAX)
						bl_list[bl_list_count++]=bl;
				}
			}
		}
	if(type&BL_MOB)
		for(by=y0/BLOCK_SIZE;by<=y1/BLOCK_SIZE;by++){
			for(bx=x0/BLOCK_SIZE;bx<=x1/BLOCK_SIZE;bx++){
				for( bl = map[m].block_mob[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
				{
					if( bl->x>=x0 && bl->x<=x1 && bl->y>=y0 && bl->y<=y1
#ifdef CIRCULAR_AREA
						&& check_distance_bl(center, bl, range)
#endif
						&& path_search_long(NULL,center->m,center->x,center->y,bl->x,bl->y,CELL_CHKWALL)
						&& bl_list_count<BL_LIST_MAX)
						bl_list[bl_list_count++]=bl;
				}
			}
		}

	if(bl_list_count>=BL_LIST_MAX)
			ShowWarning("map_foreachinrange: block count too many!\n");

	map_freeblock_lock();	// メモリからの解放を禁止する

	for(i=blockcount;i<bl_list_count;i++)
		if(bl_list[i]->prev)	// 有?かどうかチェック
		{
			va_list ap;
			va_start(ap, type);
			returnCount += func(bl_list[i], ap);
			va_end(ap);
		}

	map_freeblock_unlock();	// 解放を許可する

	bl_list_count = blockcount;
	return returnCount;	//[Skotlex]
}

/*==========================================
 * map m (x0,y0)-(x1,y1)?の全objに?して
 * funcを呼ぶ
 * type!=0 ならその種類のみ
 *------------------------------------------*/
int map_foreachinarea(int (*func)(struct block_list*,va_list), int m, int x0, int y0, int x1, int y1, int type, ...)
{
	int bx,by;
	int returnCount =0;	//total sum of returned values of func() [Skotlex]
	struct block_list *bl;
	int blockcount=bl_list_count,i;

	if (m < 0)
		return 0;
	if (x1 < x0)
	{	//Swap range
		bx = x0;
		x0 = x1;
		x1 = bx;
	}
	if (y1 < y0)
	{
		bx = y0;
		y0 = y1;
		y1 = bx;
	}
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= map[m].xs) x1 = map[m].xs-1;
	if (y1 >= map[m].ys) y1 = map[m].ys-1;
	
	if (type&~BL_MOB)
		for(by = y0 / BLOCK_SIZE; by <= y1 / BLOCK_SIZE; by++)
			for(bx = x0 / BLOCK_SIZE; bx <= x1 / BLOCK_SIZE; bx++)
				for( bl = map[m].block[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
					if(bl->type&type && bl->x>=x0 && bl->x<=x1 && bl->y>=y0 && bl->y<=y1 && bl_list_count<BL_LIST_MAX)
						bl_list[bl_list_count++]=bl;

	if(type&BL_MOB)
		for(by=y0/BLOCK_SIZE;by<=y1/BLOCK_SIZE;by++)
			for(bx=x0/BLOCK_SIZE;bx<=x1/BLOCK_SIZE;bx++)
				for( bl = map[m].block_mob[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
					if(bl->x>=x0 && bl->x<=x1 && bl->y>=y0 && bl->y<=y1 && bl_list_count<BL_LIST_MAX)
						bl_list[bl_list_count++]=bl;

	if(bl_list_count>=BL_LIST_MAX)
		ShowWarning("map_foreachinarea: block count too many!\n");

	map_freeblock_lock();	// メモリからの解放を禁止する

	for(i=blockcount;i<bl_list_count;i++)
		if(bl_list[i]->prev)	// 有?かどうかチェック
		{
			va_list ap;
			va_start(ap, type);
			returnCount += func(bl_list[i], ap);
			va_end(ap);
		}

	map_freeblock_unlock();	// 解放を許可する

	bl_list_count = blockcount;
	return returnCount;	//[Skotlex]
}

int map_forcountinarea(int (*func)(struct block_list*,va_list), int m, int x0, int y0, int x1, int y1, int count, int type, ...)
{
	int bx,by;
	int returnCount =0;	//total sum of returned values of func() [Skotlex]
	struct block_list *bl;
	int blockcount=bl_list_count,i;

	if (m < 0)
		return 0;
	if (x1 < x0)
	{	//Swap range
		bx = x0;
		x0 = x1;
		x1 = bx;
	}
	if (y1 < y0)
	{
		bx = y0;
		y0 = y1;
		y1 = bx;
	}
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= map[m].xs) x1 = map[m].xs-1;
	if (y1 >= map[m].ys) y1 = map[m].ys-1;
	
	if (type&~BL_MOB)
		for(by = y0 / BLOCK_SIZE; by <= y1 / BLOCK_SIZE; by++)
			for(bx = x0 / BLOCK_SIZE; bx <= x1 / BLOCK_SIZE; bx++)
				for( bl = map[m].block[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
					if(bl->type&type && bl->x>=x0 && bl->x<=x1 && bl->y>=y0 && bl->y<=y1 && bl_list_count<BL_LIST_MAX)
						bl_list[bl_list_count++]=bl;

	if(type&BL_MOB)
		for(by=y0/BLOCK_SIZE;by<=y1/BLOCK_SIZE;by++)
			for(bx=x0/BLOCK_SIZE;bx<=x1/BLOCK_SIZE;bx++)
				for( bl = map[m].block_mob[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
					if(bl->x>=x0 && bl->x<=x1 && bl->y>=y0 && bl->y<=y1 && bl_list_count<BL_LIST_MAX)
						bl_list[bl_list_count++]=bl;

	if(bl_list_count>=BL_LIST_MAX)
		ShowWarning("map_foreachinarea: block count too many!\n");

	map_freeblock_lock();	// メモリからの解放を禁止する

	for(i=blockcount;i<bl_list_count;i++)
		if(bl_list[i]->prev)	// 有?かどうかチェック
		{
			va_list ap;
			va_start(ap, type);
			returnCount += func(bl_list[i], ap);
			va_end(ap);
			if( count && returnCount >= count )
				break;
		}

	map_freeblock_unlock();	// 解放を許可する

	bl_list_count = blockcount;
	return returnCount;	//[Skotlex]
}

/*==========================================
 * 矩形(x0,y0)-(x1,y1)が(dx,dy)移動した暫?
 * 領域外になる領域(矩形かL字形)?のobjに
 * ?してfuncを呼ぶ
 *
 * dx,dyは-1,0,1のみとする（どんな値でもいいっぽい？）
 *------------------------------------------*/
int map_foreachinmovearea(int (*func)(struct block_list*,va_list), struct block_list* center, int range, int dx, int dy, int type, ...)
{
	int bx,by,m;
	int returnCount =0;  //total sum of returned values of func() [Skotlex]
	struct block_list *bl;
	int blockcount=bl_list_count,i;
	int x0, x1, y0, y1;

	if (!range) return 0;
	if (!dx && !dy) return 0; //No movement.
	m = center->m;

	x0 = center->x-range;
	x1 = center->x+range;
	y0 = center->y-range;
	y1 = center->y+range;

	if (x1 < x0)
	{	//Swap range
		bx = x0;
		x0 = x1;
		x1 = bx;
	}
	if (y1 < y0)
	{
		bx = y0;
		y0 = y1;
		y1 = bx;
	}
	if(dx==0 || dy==0){
		//Movement along one axis only.
		if(dx==0){
			if(dy<0) //Moving south
				y0=y1+dy+1;
			else //North
				y1=y0+dy-1;
		} else { //dy == 0
			if(dx<0) //West
				x0=x1+dx+1;
			else //East
				x1=x0+dx-1;
		}
		if(x0<0) x0=0;
		if(y0<0) y0=0;
		if(x1>=map[m].xs) x1=map[m].xs-1;
		if(y1>=map[m].ys) y1=map[m].ys-1;
		for(by=y0/BLOCK_SIZE;by<=y1/BLOCK_SIZE;by++){
			for(bx=x0/BLOCK_SIZE;bx<=x1/BLOCK_SIZE;bx++){
				if (type&~BL_MOB) {
					for( bl = map[m].block[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
					{
						if(bl->type&type &&
							bl->x>=x0 && bl->x<=x1 &&
							bl->y>=y0 && bl->y<=y1 &&
							bl_list_count<BL_LIST_MAX)
							bl_list[bl_list_count++]=bl;
					}
				}
				if (type&BL_MOB) {
					for( bl = map[m].block_mob[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
					{
						if(bl->x>=x0 && bl->x<=x1 &&
							bl->y>=y0 && bl->y<=y1 &&
							bl_list_count<BL_LIST_MAX)
							bl_list[bl_list_count++]=bl;
					}
				}
			}
		}
	}else{
		// Diagonal movement
		if(x0<0) x0=0;
		if(y0<0) y0=0;
		if(x1>=map[m].xs) x1=map[m].xs-1;
		if(y1>=map[m].ys) y1=map[m].ys-1;
		for(by=y0/BLOCK_SIZE;by<=y1/BLOCK_SIZE;by++){
			for(bx=x0/BLOCK_SIZE;bx<=x1/BLOCK_SIZE;bx++){
				if (type & ~BL_MOB) {
					for( bl = map[m].block[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
					{
						if( bl->type&type &&
							bl->x>=x0 && bl->x<=x1 &&
							bl->y>=y0 && bl->y<=y1 &&
							bl_list_count<BL_LIST_MAX )
						if((dx>0 && bl->x<x0+dx) ||
							(dx<0 && bl->x>x1+dx) ||
							(dy>0 && bl->y<y0+dy) ||
							(dy<0 && bl->y>y1+dy))
							bl_list[bl_list_count++]=bl;
					}
				}
				if (type & BL_MOB) {
					for( bl = map[m].block_mob[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
					{
						if( bl->x>=x0 && bl->x<=x1 &&
							bl->y>=y0 && bl->y<=y1 &&
							bl_list_count<BL_LIST_MAX)
						if((dx>0 && bl->x<x0+dx) ||
							(dx<0 && bl->x>x1+dx) ||
							(dy>0 && bl->y<y0+dy) ||
							(dy<0 && bl->y>y1+dy))
							bl_list[bl_list_count++]=bl;
					}
				}
			}
		}

	}

	if(bl_list_count>=BL_LIST_MAX)
		ShowWarning("map_foreachinmovearea: block count too many!\n");

	map_freeblock_lock();	// メモリからの解放を禁止する

	for(i=blockcount;i<bl_list_count;i++)
		if(bl_list[i]->prev)
		{
			va_list ap;
			va_start(ap, type);
			returnCount += func(bl_list[i], ap);
			va_end(ap);
		}

	map_freeblock_unlock();	// 解放を許可する

	bl_list_count = blockcount;
	return returnCount;
}

// -- moonsoul	(added map_foreachincell which is a rework of map_foreachinarea but
//			 which only checks the exact single x/y passed to it rather than an
//			 area radius - may be more useful in some instances)
//
int map_foreachincell(int (*func)(struct block_list*,va_list), int m, int x, int y, int type, ...)
{
	int bx,by;
	int returnCount =0;  //total sum of returned values of func() [Skotlex]
	struct block_list *bl;
	int blockcount=bl_list_count,i;

	if (x < 0 || y < 0 || x >= map[m].xs || y >= map[m].ys) return 0;

	by=y/BLOCK_SIZE;
	bx=x/BLOCK_SIZE;

	if(type&~BL_MOB)
		for( bl = map[m].block[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
			if(bl->type&type && bl->x==x && bl->y==y && bl_list_count<BL_LIST_MAX)
				bl_list[bl_list_count++]=bl;

	if(type&BL_MOB)
		for( bl = map[m].block_mob[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
			if(bl->x==x && bl->y==y && bl_list_count<BL_LIST_MAX)
				bl_list[bl_list_count++]=bl;

	if(bl_list_count>=BL_LIST_MAX)
		ShowWarning("map_foreachincell: block count too many!\n");

	map_freeblock_lock();	// メモリからの解放を禁止する

	for(i=blockcount;i<bl_list_count;i++)
		if(bl_list[i]->prev)	// 有?かどうかチェック
		{
			va_list ap;
			va_start(ap, type);
			returnCount += func(bl_list[i], ap);
			va_end(ap);
		}

	map_freeblock_unlock();	// 解放を許可する

	bl_list_count = blockcount;
	return returnCount;
}

/*============================================================
* For checking a path between two points (x0, y0) and (x1, y1)
*------------------------------------------------------------*/
int map_foreachinpath(int (*func)(struct block_list*,va_list),int m,int x0,int y0,int x1,int y1,int range,int length, int type,...)
{
	int returnCount =0;  //total sum of returned values of func() [Skotlex]
//////////////////////////////////////////////////////////////
//
// sharp shooting 3 [Skotlex]
//
//////////////////////////////////////////////////////////////
// problem:
// Same as Sharp Shooting 1. Hits all targets within range of
// the line.
// (t1,t2 t3 and t4 get hit)
//
//     target 1
//      x t4
//     t2
// t3 x
//   x
//  S
//////////////////////////////////////////////////////////////
// Methodology: 
// My trigonometrics and math are a little rusty... so the approach I am writing 
// here is basicly do a double for to check for all targets in the square that 
// contains the initial and final positions (area range increased to match the 
// radius given), then for each object to test, calculate the distance to the 
// path and include it if the range fits and the target is in the line (0<k<1,
// as they call it).
// The implementation I took as reference is found at 
// http://astronomy.swin.edu.au/~pbourke/geometry/pointline/ 
// (they have a link to a C implementation, too)
// This approach is a lot like #2 commented on this function, which I have no 
// idea why it was commented. I won't use doubles/floats, but pure int math for
// speed purposes. The range considered is always the same no matter how 
// close/far the target is because that's how SharpShooting works currently in
// kRO.

	//Generic map_foreach* variables.
	int i, blockcount = bl_list_count;
	struct block_list *bl;
	int bx, by;
	//method specific variables
	int magnitude2, len_limit; //The square of the magnitude
	int k, xi, yi, xu, yu;
	int mx0 = x0, mx1 = x1, my0 = y0, my1 = y1;
	
	//Avoid needless calculations by not getting the sqrt right away.
	#define MAGNITUDE2(x0, y0, x1, y1) (((x1)-(x0))*((x1)-(x0)) + ((y1)-(y0))*((y1)-(y0)))
	
	if (m < 0)
		return 0;

	len_limit = magnitude2 = MAGNITUDE2(x0,y0, x1,y1);
	if (magnitude2 < 1) //Same begin and ending point, can't trace path.
		return 0;

	if (length)
	{	//Adjust final position to fit in the given area.
		//TODO: Find an alternate method which does not requires a square root calculation.
		k = (int)sqrt((float)magnitude2);
		mx1 = x0 + (x1 - x0)*length/k;
		my1 = y0 + (y1 - y0)*length/k;
		len_limit = MAGNITUDE2(x0,y0, mx1,my1);
	}
	//Expand target area to cover range.
	if (mx0 > mx1)
	{
		mx0+=range;
		mx1-=range;
	} else {
		mx0-=range;
		mx1+=range;
	}
	if (my0 > my1)
	{
		my0+=range;
		my1-=range;
	} else {
		my0-=range;
		my1+=range;
	}

	//The two fors assume mx0 < mx1 && my0 < my1
	if (mx0 > mx1)
	{
		k = mx1;
		mx1 = mx0;
		mx0 = k;
	}
	if (my0 > my1)
	{
		k = my1;
		my1 = my0;
		my0 = k;
	}
	
	if (mx0 < 0) mx0 = 0;
	if (my0 < 0) my0 = 0;
	if (mx1 >= map[m].xs) mx1 = map[m].xs-1;
	if (my1 >= map[m].ys) my1 = map[m].ys-1;
	
	range*=range<<8; //Values are shifted later on for higher precision using int math.
	
	if (type & ~BL_MOB)
		for (by = my0 / BLOCK_SIZE; by <= my1 / BLOCK_SIZE; by++) {
			for(bx=mx0/BLOCK_SIZE;bx<=mx1/BLOCK_SIZE;bx++){
				for( bl = map[m].block[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
				{
					if(bl->prev && bl->type&type && bl_list_count<BL_LIST_MAX)
					{
						xi = bl->x;
						yi = bl->y;
					
						k = (xi-x0)*(x1-x0) + (yi-y0)*(y1-y0);
						if (k < 0 || k > len_limit) //Since more skills use this, check for ending point as well.
							continue;
						
						if (k > magnitude2 && !path_search_long(NULL,m,x0,y0,xi,yi,CELL_CHKWALL))
							continue; //Targets beyond the initial ending point need the wall check.

						//All these shifts are to increase the precision of the intersection point and distance considering how it's
						//int math.
						k = (k<<4)/magnitude2; //k will be between 1~16 instead of 0~1
						xi<<=4;
						yi<<=4;
						xu= (x0<<4) +k*(x1-x0);
						yu= (y0<<4) +k*(y1-y0);
						k = MAGNITUDE2(xi, yi, xu, yu);
						
						//If all dot coordinates were <<4 the square of the magnitude is <<8
						if (k > range)
							continue;

						bl_list[bl_list_count++]=bl;
					}
				}
			}
		}

	if(type&BL_MOB)
		for(by=my0/BLOCK_SIZE;by<=my1/BLOCK_SIZE;by++){
			for(bx=mx0/BLOCK_SIZE;bx<=mx1/BLOCK_SIZE;bx++){
				for( bl = map[m].block_mob[bx+by*map[m].bxs] ; bl != NULL ; bl = bl->next )
				{
					if(bl->prev && bl_list_count<BL_LIST_MAX)
					{
						xi = bl->x;
						yi = bl->y;
						k = (xi-x0)*(x1-x0) + (yi-y0)*(y1-y0);
						if (k < 0 || k > len_limit)
							continue;
				
						if (k > magnitude2 && !path_search_long(NULL,m,x0,y0,xi,yi,CELL_CHKWALL))
							continue; //Targets beyond the initial ending point need the wall check.
	
						k = (k<<4)/magnitude2; //k will be between 1~16 instead of 0~1
						xi<<=4;
						yi<<=4;
						xu= (x0<<4) +k*(x1-x0);
						yu= (y0<<4) +k*(y1-y0);
						k = MAGNITUDE2(xi, yi, xu, yu);
						
						//If all dot coordinates were <<4 the square of the magnitude is <<8
						if (k > range)
							continue;

						bl_list[bl_list_count++]=bl;
					}
				}
			}
		}

	if(bl_list_count>=BL_LIST_MAX)
		ShowWarning("map_foreachinpath: block count too many!\n");

	map_freeblock_lock();

	for(i=blockcount;i<bl_list_count;i++)
		if(bl_list[i]->prev)	//This check is done in case some object gets killed due to further skill processing.
		{
			va_list ap;
			va_start(ap, type);
			returnCount += func(bl_list[i], ap);
			va_end(ap);
		}

	map_freeblock_unlock();

	bl_list_count = blockcount;
	return returnCount;	//[Skotlex]

}

// Copy of map_foreachincell, but applied to the whole map. [Skotlex]
int map_foreachinmap(int (*func)(struct block_list*,va_list), int m, int type,...)
{
	int b, bsize;
	int returnCount =0;  //total sum of returned values of func() [Skotlex]
	struct block_list *bl;
	int blockcount=bl_list_count,i;

	bsize = map[m].bxs * map[m].bys;

	if(type&~BL_MOB)
		for(b=0;b<bsize;b++)
			for( bl = map[m].block[b] ; bl != NULL ; bl = bl->next )
				if(bl->type&type && bl_list_count<BL_LIST_MAX)
					bl_list[bl_list_count++]=bl;

	if(type&BL_MOB)
		for(b=0;b<bsize;b++)
			for( bl = map[m].block_mob[b] ; bl != NULL ; bl = bl->next )
				if(bl_list_count<BL_LIST_MAX)
					bl_list[bl_list_count++]=bl;

	if(bl_list_count>=BL_LIST_MAX)
		ShowWarning("map_foreachinmap: block count too many!\n");

	map_freeblock_lock();	// メモリからの解放を禁止する

	for(i=blockcount;i<bl_list_count;i++)
		if(bl_list[i]->prev)	// 有?かどうかチェック
		{
			va_list ap;
			va_start(ap, type);
			returnCount += func(bl_list[i], ap);
			va_end(ap);
		}

	map_freeblock_unlock();	// 解放を許可する

	bl_list_count = blockcount;
	return returnCount;
}


/// Generates a new flooritem object id from the interval [MIN_FLOORITEM, MAX_FLOORITEM).
/// Used for floor items, skill units and chatroom objects.
/// @return The new object id
int map_get_new_object_id(void)
{
	static int last_object_id = MIN_FLOORITEM - 1;
	int i;

	// find a free id
	i = last_object_id + 1;
	while( i != last_object_id )
	{
		if( i == MAX_FLOORITEM )
			i = MIN_FLOORITEM;

		if( idb_get(id_db, i) == NULL )
			break;

		++i;
	}

	if( i == last_object_id )
	{
		ShowError("map_addobject: no free object id!\n");
		return 0;
	}

	// update cursor
	last_object_id = i;

	return i;
}

/*==========================================
 * 床アイテムを消す
 *
 * data==0の暫ﾍtimerで消えた殊 * data!=0の暫ﾍ拾う等で消えた暫ﾆして動�?
 *
 * 後者は、map_clearflooritem(id)へ
 * map.h?で#defineしてある
 *------------------------------------------*/
int map_clearflooritem_timer(int tid, unsigned int tick, int id, intptr data)
{
	struct flooritem_data* fitem = (struct flooritem_data*)idb_get(id_db, id);
	if( fitem==NULL || fitem->bl.type!=BL_ITEM || (!data && fitem->cleartimer != tid) )
	{
		ShowError("map_clearflooritem_timer : error\n");
		return 1;
	}

	if(data)
		delete_timer(fitem->cleartimer,map_clearflooritem_timer);
	else
	if(fitem->item_data.card[0] == CARD0_PET) // pet egg
		intif_delete_petdata( MakeDWord(fitem->item_data.card[1],fitem->item_data.card[2]) );

	clif_clearflooritem(fitem,0);
	map_deliddb(&fitem->bl);
	map_delblock(&fitem->bl);
	map_freeblock(&fitem->bl);

	return 0;
}

/*==========================================
 * (m,x,y) locates a random available free cell around the given coordinates
 * to place an BL_ITEM object. Scan area is 9x9, returns 1 on success.
 * x and y are modified with the target cell when successful.
 *------------------------------------------*/
int map_searchrandfreecell(int m,int *x,int *y,int stack)
{
	int free_cell,i,j;
	int free_cells[9][2];

	for(free_cell=0,i=-1;i<=1;i++){
		if(i+*y<0 || i+*y>=map[m].ys)
			continue;
		for(j=-1;j<=1;j++){
			if(j+*x<0 || j+*x>=map[m].xs)
				continue;
			if(map_getcell(m,j+*x,i+*y,CELL_CHKNOPASS))
				continue;
			//Avoid item stacking to prevent against exploits. [Skotlex]
			if(stack && map_count_oncell(m,j+*x,i+*y, BL_ITEM) > stack)
				continue;
			free_cells[free_cell][0] = j+*x;
			free_cells[free_cell++][1] = i+*y;
		}
	}
	if(free_cell==0)
		return 0;
	free_cell = rand()%free_cell;
	*x = free_cells[free_cell][0];
	*y = free_cells[free_cell][1];
	return 1;
}


static int map_count_sub(struct block_list *bl,va_list ap)
{
	return 1;
}

/*==========================================
 * Locates a random spare cell around the object given, using range as max 
 * distance from that spot. Used for warping functions. Use range < 0 for 
 * whole map range.
 * Returns 1 on success. when it fails and src is available, x/y are set to src's
 * src can be null as long as flag&1
 * when ~flag&1, m is not needed.
 * Flag values:
 * &1 = random cell must be around given m,x,y, not around src
 * &2 = the target should be able to walk to the target tile.
 * &4 = there shouldn't be any players around the target tile (use the no_spawn_on_player setting)
 *------------------------------------------*/
int map_search_freecell(struct block_list *src, int m, short *x,short *y, int rx, int ry, int flag)
{
	int tries, spawn=0;
	int bx, by;
	int rx2 = 2*rx+1;
	int ry2 = 2*ry+1;

	if( !src && (!(flag&1) || flag&2) )
	{
		ShowDebug("map_search_freecell: Incorrect usage! When src is NULL, flag has to be &1 and can't have &2\n");
		return 0;
	}

	if (flag&1) {
		bx = *x;
		by = *y;
	} else {
		bx = src->x;
		by = src->y;
		m = src->m;
	}
	if (!rx && !ry) {
		//No range? Return the target cell then....
		*x = bx;
		*y = by;
		return map_getcell(m,*x,*y,CELL_CHKREACH);
	}
	
	if (rx >= 0 && ry >= 0) {
		tries = rx2*ry2;
		if (tries > 100) tries = 100;
	} else {
		tries = map[m].xs*map[m].ys;
		if (tries > 500) tries = 500;
	}
	
	while(tries--) {
		*x = (rx >= 0)?(rand()%rx2-rx+bx):(rand()%(map[m].xs-2)+1);
		*y = (ry >= 0)?(rand()%ry2-ry+by):(rand()%(map[m].ys-2)+1);
		
		if (*x == bx && *y == by)
			continue; //Avoid picking the same target tile.
		
		if (map_getcell(m,*x,*y,CELL_CHKREACH))
		{
			if(flag&2 && !unit_can_reach_pos(src, *x, *y, 1))
				continue;
			if(flag&4) {
				if (spawn >= 100) return 0; //Limit of retries reached.
				if (spawn++ < battle_config.no_spawn_on_player &&
					map_foreachinarea(map_count_sub, m,
						*x-AREA_SIZE, *y-AREA_SIZE,
					  	*x+AREA_SIZE, *y+AREA_SIZE, BL_PC)
				)
				continue;
			}
			return 1;
		}
	}
	*x = bx;
	*y = by;
	return 0;
}

/*==========================================
 * (m,x,y)を中心に3x3以?に床アイテム設置
 *
 * item_dataはamount以外をcopyする
 * type flag: &1 MVP item. &2 do stacking check.
 *------------------------------------------*/
int map_addflooritem(struct item *item_data,int amount,int m,int x,int y,int first_charid,int second_charid,int third_charid,int flags)
{
	int r;
	struct flooritem_data *fitem=NULL;

	nullpo_retr(0, item_data);

	if(!map_searchrandfreecell(m,&x,&y,flags&2?1:0))
		return 0;
	r=rand();

	CREATE(fitem, struct flooritem_data, 1);
	fitem->bl.type=BL_ITEM;
	fitem->bl.prev = fitem->bl.next = NULL;
	fitem->bl.m=m;
	fitem->bl.x=x;
	fitem->bl.y=y;
	fitem->bl.id = map_get_new_object_id();
	if(fitem->bl.id==0){
		aFree(fitem);
		return 0;
	}

	fitem->first_get_charid = first_charid;
	fitem->first_get_tick = gettick() + (flags&1 ? battle_config.mvp_item_first_get_time : battle_config.item_first_get_time);
	fitem->second_get_charid = second_charid;
	fitem->second_get_tick = fitem->first_get_tick + (flags&1 ? battle_config.mvp_item_second_get_time : battle_config.item_second_get_time);
	fitem->third_get_charid = third_charid;
	fitem->third_get_tick = fitem->second_get_tick + (flags&1 ? battle_config.mvp_item_third_get_time : battle_config.item_third_get_time);

	memcpy(&fitem->item_data,item_data,sizeof(*item_data));
	fitem->item_data.amount=amount;
	fitem->subx=(r&3)*3+3;
	fitem->suby=((r>>2)&3)*3+3;
	fitem->cleartimer=add_timer(gettick()+battle_config.flooritem_lifetime,map_clearflooritem_timer,fitem->bl.id,0);

	map_addiddb(&fitem->bl);
	map_addblock(&fitem->bl);
	clif_dropflooritem(fitem);

	return fitem->bl.id;
}

static void* create_charid2nick(DBKey key, va_list args)
{
	struct charid2nick *p;
	CREATE(p, struct charid2nick, 1);
	return p;
}

/// Adds(or replaces) the nick of charid to nick_db and fullfils pending requests.
/// Does nothing if the character is online.
void map_addnickdb(int charid, const char* nick)
{
	struct charid2nick* p;
	struct charid_request* req;
	struct map_session_data* sd;

	if( map_charid2sd(charid) )
		return;// already online

	p = (struct charid2nick*)idb_ensure(nick_db, charid, create_charid2nick);
	safestrncpy(p->nick, nick, sizeof(p->nick));

	while( p->requests )
	{
		req = p->requests;
		p->requests = req->next;
		sd = map_charid2sd(req->charid);
		if( sd )
			clif_solved_charname(sd->fd, charid, p->nick);
		aFree(req);
	}
}

/// Removes the nick of charid from nick_db.
/// Sends name to all pending requests on charid.
void map_delnickdb(int charid, const char* name)
{
	struct charid2nick* p;
	struct charid_request* req;
	struct map_session_data* sd;

	p = (struct charid2nick*)idb_remove(nick_db, charid);
	if( p == NULL )
		return;

	while( p->requests )
	{
		req = p->requests;
		p->requests = req->next;
		sd = map_charid2sd(req->charid);
		if( sd )
			clif_solved_charname(sd->fd, charid, name);
		aFree(req);
	}
	aFree(p);
}

/// Notifies sd of the nick of charid.
/// Uses the name in the character if online.
/// Uses the name in nick_db if offline.
void map_reqnickdb(struct map_session_data * sd, int charid)
{
	struct charid2nick* p;
	struct charid_request* req;
	struct map_session_data* tsd;

	nullpo_retv(sd);

	tsd = map_charid2sd(charid);
	if( tsd )
	{
		clif_solved_charname(sd->fd, charid, tsd->status.name);
		return;
	}

	p = (struct charid2nick*)idb_ensure(nick_db, charid, create_charid2nick);
	if( *p->nick )
	{
		clif_solved_charname(sd->fd, charid, p->nick);
		return;
	}
	// not in cache, request it
	CREATE(req, struct charid_request, 1);
	req->next = p->requests;
	p->requests = req;
	chrif_searchcharid(charid);
}

/*==========================================
 * id_dbへblを追加
 *------------------------------------------*/
void map_addiddb(struct block_list *bl)
{
	nullpo_retv(bl);

	if( bl->type == BL_PC )
	{
		TBL_PC* sd = (TBL_PC*)bl;
		idb_put(pc_db,sd->bl.id,sd);
		idb_put(charid_db,sd->status.char_id,sd);
	}
	else if( bl->type == BL_MOB )
	{
		TBL_MOB* md = (TBL_MOB*)bl;
		idb_put(mobid_db,bl->id,bl);

		if( md->boss )
			idb_put(bossid_db, bl->id, bl);
	}

	if( bl->type & BL_REGEN )
		idb_put(regen_db, bl->id, bl);

	idb_put(id_db,bl->id,bl);
}

/*==========================================
 * id_dbからblを削除
 *------------------------------------------*/
void map_deliddb(struct block_list *bl)
{
	nullpo_retv(bl);

	if( bl->type == BL_PC )
	{
		TBL_PC* sd = (TBL_PC*)bl;
		idb_remove(pc_db,sd->bl.id);
		idb_remove(charid_db,sd->status.char_id);
	}
	else if( bl->type == BL_MOB )
	{
		idb_remove(mobid_db,bl->id);
		idb_remove(bossid_db,bl->id);
	}

	if( bl->type & BL_REGEN )
		idb_remove(regen_db,bl->id);

	idb_remove(id_db,bl->id);
}

/*==========================================
 * Standard call when a player connection is closed.
 *------------------------------------------*/
int map_quit(struct map_session_data *sd)
{
	if(!sd->state.active) { //Removing a player that is not active.
		struct auth_node *node = chrif_search(sd->status.account_id);
		if (node && node->char_id == sd->status.char_id &&
			node->state != ST_LOGOUT)
			//Except when logging out, clear the auth-connect data immediately.
			chrif_auth_delete(node->account_id, node->char_id, node->state);
		//Non-active players should not have loaded any data yet (or it was cleared already) so no additional cleanups are needed.
		return 0;
	}

	if (sd->npc_timer_id != -1) //Cancel the event timer.
		npc_timerevent_quit(sd);

	if (sd->npc_id)
		npc_event_dequeue(sd);

	npc_script_event(sd, NPCE_LOGOUT);

	//Unit_free handles clearing the player related data, 
	//map_quit handles extra specific data which is related to quitting normally
	//(changing map-servers invokes unit_free but bypasses map_quit)
	if( sd->sc.count )
	{
		//Status that are not saved...
		if(sd->sc.data[SC_BOSSMAPINFO])
			status_change_end(&sd->bl,SC_BOSSMAPINFO,-1);
		if(sd->sc.data[SC_AUTOTRADE])
			status_change_end(&sd->bl,SC_AUTOTRADE,-1);
		if(sd->sc.data[SC_SPURT])
			status_change_end(&sd->bl,SC_SPURT,-1);
		if(sd->sc.data[SC_BERSERK])
			status_change_end(&sd->bl,SC_BERSERK,-1);
		if(sd->sc.data[SC_TRICKDEAD])
			status_change_end(&sd->bl,SC_TRICKDEAD,-1);
		if(sd->sc.data[SC_GUILDAURA])
			status_change_end(&sd->bl,SC_GUILDAURA,-1);
		if(sd->sc.data[SC_ENDURE] && sd->sc.data[SC_ENDURE]->val4)
			status_change_end(&sd->bl,SC_ENDURE,-1); //No need to save infinite endure.
		if(sd->sc.data[SC_WEIGHT50])
			status_change_end(&sd->bl,SC_WEIGHT50,-1);
		if(sd->sc.data[SC_WEIGHT90])
			status_change_end(&sd->bl,SC_WEIGHT90,-1);
		if (battle_config.debuff_on_logout&1) {
			if(sd->sc.data[SC_ORCISH])
				status_change_end(&sd->bl,SC_ORCISH,-1);
			if(sd->sc.data[SC_STRIPWEAPON])
				status_change_end(&sd->bl,SC_STRIPWEAPON,-1);
			if(sd->sc.data[SC_STRIPARMOR])
				status_change_end(&sd->bl,SC_STRIPARMOR,-1);
			if(sd->sc.data[SC_STRIPSHIELD])
				status_change_end(&sd->bl,SC_STRIPSHIELD,-1);
			if(sd->sc.data[SC_STRIPHELM])
				status_change_end(&sd->bl,SC_STRIPHELM,-1);
			if(sd->sc.data[SC_EXTREMITYFIST])
				status_change_end(&sd->bl,SC_EXTREMITYFIST,-1);
			if(sd->sc.data[SC_EXPLOSIONSPIRITS])
				status_change_end(&sd->bl,SC_EXPLOSIONSPIRITS,-1);
			if(sd->sc.data[SC_REGENERATION] && sd->sc.data[SC_REGENERATION]->val4)
				status_change_end(&sd->bl,SC_REGENERATION,-1);
			//TO-DO Probably there are way more NPC_type negative status that are removed
			if(sd->sc.data[SC_CHANGEUNDEAD])
				status_change_end(&sd->bl,SC_CHANGEUNDEAD,-1);
			// Both these statuses are removed on logout. [L0ne_W0lf]
			if(sd->sc.data[SC_SLOWCAST])
				status_change_end(&sd->bl,SC_SLOWCAST,-1);
			if(sd->sc.data[SC_CRITICALWOUND])
				status_change_end(&sd->bl,SC_CRITICALWOUND,-1);
		}
		if (battle_config.debuff_on_logout&2)
		{
			if(sd->sc.data[SC_MAXIMIZEPOWER])
				status_change_end(&sd->bl,SC_MAXIMIZEPOWER,-1);
			if(sd->sc.data[SC_MAXOVERTHRUST])
				status_change_end(&sd->bl,SC_MAXOVERTHRUST,-1);
			if(sd->sc.data[SC_STEELBODY])
				status_change_end(&sd->bl,SC_STEELBODY,-1);
			if(sd->sc.data[SC_PRESERVE])
				status_change_end(&sd->bl,SC_PRESERVE,-1);
			if(sd->sc.data[SC_KAAHI])
				status_change_end(&sd->bl,SC_KAAHI,-1);
			if(sd->sc.data[SC_SPIRIT])
				status_change_end(&sd->bl,SC_SPIRIT,-1);
		}
	}
	
	// Return loot to owner
	if( sd->pd ) pet_lootitem_drop(sd->pd, sd);
	if( sd->state.storage_flag == 1 ) sd->state.storage_flag = 0; // No need to Double Save Storage on Quit.

	unit_remove_map_pc(sd,3);
	
	if( map[sd->bl.m].instance_id )
	{ // Avoid map conflicts and warnings on next login
		int m;
		struct point *pt;
		if( map[sd->bl.m].save.map )
			pt = &map[sd->bl.m].save;
		else
			pt = &sd->status.save_point;

		if( (m=map_mapindex2mapid(pt->map)) >= 0 )
		{
			sd->bl.m = m;
			sd->bl.x = pt->x;
			sd->bl.y = pt->y;
			sd->mapindex = pt->map;
		}
	}	

	pc_makesavestatus(sd);
	pc_clean_skilltree(sd);
	chrif_save(sd,1);
	unit_free_pc(sd);
	return 0;
}

/*==========================================
 * id番?のPCを探す。居なければNULL
 *------------------------------------------*/
struct map_session_data * map_id2sd(int id)
{
	if (id <= 0) return NULL;
	return (struct map_session_data*)idb_get(pc_db,id);
}

struct mob_data * map_id2md(int id)
{
	if (id <= 0) return NULL;
	return (struct mob_data*)idb_get(mobid_db,id);
}

struct npc_data * map_id2nd(int id)
{// just a id2bl lookup because there's no npc_db
	if (id <= 0) return NULL;
	return (struct npc_data*)map_id2bl(id);
}

/// Returns the nick of the target charid or NULL if unknown (requests the nick to the char server).
const char* map_charid2nick(int charid)
{
	struct charid2nick *p;
	struct map_session_data* sd;

	sd = map_charid2sd(charid);
	if( sd )
		return sd->status.name;// character is online, return it's name

	p = (struct charid2nick*)idb_ensure(nick_db, charid, create_charid2nick);
	if( *p->nick )
		return p->nick;// name in nick_db

	chrif_searchcharid(charid);// request the name
	return NULL;
}

/// Returns the struct map_session_data of the charid or NULL if the char is not online.
struct map_session_data* map_charid2sd(int charid)
{
	return (struct map_session_data*)idb_get(charid_db, charid);
}

/*==========================================
 * Search session data from a nick name
 * (without sensitive case if necessary)
 * return map_session_data pointer or NULL
 *------------------------------------------*/
struct map_session_data * map_nick2sd(const char *nick)
{
	struct map_session_data* sd;
	struct map_session_data* found_sd;
	struct s_mapiterator* iter;
	size_t nicklen;
	int qty = 0;

	if( nick == NULL )
		return NULL;

	nicklen = strlen(nick);
	iter = mapit_getallusers();

	found_sd = NULL;
	for( sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC*)mapit_next(iter) )
	{
		if( battle_config.partial_name_scan )
		{// partial name search
			if( strnicmp(sd->status.name, nick, nicklen) == 0 )
			{
				found_sd = sd;

				if( strcmp(sd->status.name, nick) == 0 )
				{// Perfect Match
					qty = 1;
					break;
				}

				qty++;
			}
		}
		else if( strcasecmp(sd->status.name, nick) == 0 )
		{// exact search only
			found_sd = sd;
			break;
		}
	}
	mapit_free(iter);

	if( battle_config.partial_name_scan && qty != 1 )
		found_sd = NULL;

	return found_sd;
}

/*==========================================
 * id番?の物を探す
 * 一三bjectの場合は配列を引くのみ
 *------------------------------------------*/
struct block_list * map_id2bl(int id)
{
	return (struct block_list*)idb_get(id_db,id);
}

/*==========================================
 * Convext Mirror
 *------------------------------------------*/
struct mob_data * map_getmob_boss(int m)
{
	DBIterator* iter;
	struct mob_data *md = NULL;
	bool found = false;

	iter = db_iterator(bossid_db);
	for( md = (struct mob_data*)dbi_first(iter); dbi_exists(iter); md = (struct mob_data*)dbi_next(iter) )
	{
		if( md->bl.m == m )
		{
			found = true;		
			break;
		}
	}
	dbi_destroy(iter);

	return (found)? md : NULL;
}

struct mob_data * map_id2boss(int id)
{
	if (id <= 0) return NULL;
	return (struct mob_data*)idb_get(bossid_db,id);
}

/// Applies func to all the players in the db.
/// Stops iterating if func returns -1.
void map_foreachpc(int (*func)(struct map_session_data* sd, va_list args), ...)
{
	DBIterator* iter;
	struct map_session_data* sd;

	iter = db_iterator(pc_db);
	for( sd = (struct map_session_data*)iter->first(iter,NULL); iter->exists(iter); sd = (struct map_session_data*)iter->next(iter,NULL) )
	{
		va_list args;
		int ret;

		va_start(args, func);
		ret = func(sd, args);
		va_end(args);
		if( ret == -1 )
			break;// stop iterating
	}
	dbi_destroy(iter);
}

/// Applies func to all the mobs in the db.
/// Stops iterating if func returns -1.
void map_foreachmob(int (*func)(struct mob_data* md, va_list args), ...)
{
	DBIterator* iter;
	struct mob_data* md;

	iter = db_iterator(mobid_db);
	for( md = (struct mob_data*)dbi_first(iter); dbi_exists(iter); md = (struct mob_data*)dbi_next(iter) )
	{
		va_list args;
		int ret;

		va_start(args, func);
		ret = func(md, args);
		va_end(args);
		if( ret == -1 )
			break;// stop iterating
	}
	dbi_destroy(iter);
}

/// Applies func to all the npcs in the db.
/// Stops iterating if func returns -1.
void map_foreachnpc(int (*func)(struct npc_data* nd, va_list args), ...)
{
	DBIterator* iter;
	struct block_list* bl;

	iter = db_iterator(id_db);
	for( bl = (struct block_list*)dbi_first(iter); dbi_exists(iter); bl = (struct block_list*)dbi_next(iter) )
	{
		if( bl->type == BL_NPC )
		{
			struct npc_data* nd = (struct npc_data*)bl;
			va_list args;
			int ret;

			va_start(args, func);
			ret = func(nd, args);
			va_end(args);
			if( ret == -1 )
				break;// stop iterating
		}
	}
	dbi_destroy(iter);
}

/// Applies func to everything in the db.
/// Stops iteratin gif func returns -1.
void map_foreachregen(int (*func)(struct block_list* bl, va_list args), ...)
{
	DBIterator* iter;
	struct block_list* bl;

	iter = db_iterator(regen_db);
	for( bl = (struct block_list*)dbi_first(iter); dbi_exists(iter); bl = (struct block_list*)dbi_next(iter) )
	{
		va_list args;
		int ret;

		va_start(args, func);
		ret = func(bl, args);
		va_end(args);
		if( ret == -1 )
			break;// stop iterating
	}
	dbi_destroy(iter);
}

/// Applies func to everything in the db.
/// Stops iterating if func returns -1.
void map_foreachiddb(int (*func)(struct block_list* bl, va_list args), ...)
{
	DBIterator* iter;
	struct block_list* bl;

	iter = db_iterator(id_db);
	for( bl = (struct block_list*)dbi_first(iter); dbi_exists(iter); bl = (struct block_list*)dbi_next(iter) )
	{
		va_list args;
		int ret;

		va_start(args, func);
		ret = func(bl, args);
		va_end(args);
		if( ret == -1 )
			break;// stop iterating
	}
	dbi_destroy(iter);
}

/// Iterator.
/// Can filter by bl type.
struct s_mapiterator
{
	enum e_mapitflags flags;// flags for special behaviour
	enum bl_type types;// what bl types to return
	DBIterator* dbi;// database iterator
};

/// Returns true if the block_list matches the description in the iterator.
///
/// @param _mapit_ Iterator
/// @param _bl_ block_list
/// @return true if it matches
#define MAPIT_MATCHES(_mapit_,_bl_) \
	( \
		( (_bl_)->type & (_mapit_)->types /* type matches */ ) \
	)

/// Allocates a new iterator.
/// Returns the new iterator.
/// types can represent several BL's as a bit field.
/// TODO should this be expanded to allow filtering of map/guild/party/chat/cell/area/...?
///
/// @param flags Flags of the iterator
/// @param type Target types
/// @return Iterator
struct s_mapiterator* mapit_alloc(enum e_mapitflags flags, enum bl_type types)
{
	struct s_mapiterator* mapit;

	CREATE(mapit, struct s_mapiterator, 1);
	mapit->flags = flags;
	mapit->types = types;
	if( types == BL_PC )       mapit->dbi = db_iterator(pc_db);
	else if( types == BL_MOB ) mapit->dbi = db_iterator(mobid_db);
	else                       mapit->dbi = db_iterator(id_db);
	return mapit;
}

/// Frees the iterator.
///
/// @param mapit Iterator
void mapit_free(struct s_mapiterator* mapit)
{
	nullpo_retv(mapit);

	dbi_destroy(mapit->dbi);
	aFree(mapit);
}

/// Returns the first block_list that matches the description.
/// Returns NULL if not found.
///
/// @param mapit Iterator
/// @return first block_list or NULL
struct block_list* mapit_first(struct s_mapiterator* mapit)
{
	struct block_list* bl;

	nullpo_retr(NULL,mapit);

	for( bl = (struct block_list*)dbi_first(mapit->dbi); bl != NULL; bl = (struct block_list*)dbi_next(mapit->dbi) )
	{
		if( MAPIT_MATCHES(mapit,bl) )
			break;// found match
	}
	return bl;
}

/// Returns the last block_list that matches the description.
/// Returns NULL if not found.
///
/// @param mapit Iterator
/// @return last block_list or NULL
struct block_list* mapit_last(struct s_mapiterator* mapit)
{
	struct block_list* bl;

	nullpo_retr(NULL,mapit);

	for( bl = (struct block_list*)dbi_last(mapit->dbi); bl != NULL; bl = (struct block_list*)dbi_prev(mapit->dbi) )
	{
		if( MAPIT_MATCHES(mapit,bl) )
			break;// found match
	}
	return bl;
}

/// Returns the next block_list that matches the description.
/// Returns NULL if not found.
///
/// @param mapit Iterator
/// @return next block_list or NULL
struct block_list* mapit_next(struct s_mapiterator* mapit)
{
	struct block_list* bl;

	nullpo_retr(NULL,mapit);

	for( ; ; )
	{
		bl = (struct block_list*)dbi_next(mapit->dbi);
		if( bl == NULL )
			break;// end
		if( MAPIT_MATCHES(mapit,bl) )
			break;// found a match
		// try next
	}
	return bl;
}

/// Returns the previous block_list that matches the description.
/// Returns NULL if not found.
///
/// @param mapit Iterator
/// @return previous block_list or NULL
struct block_list* mapit_prev(struct s_mapiterator* mapit)
{
	struct block_list* bl;

	nullpo_retr(NULL,mapit);

	for( ; ; )
	{
		bl = (struct block_list*)dbi_prev(mapit->dbi);
		if( bl == NULL )
			break;// end
		if( MAPIT_MATCHES(mapit,bl) )
			break;// found a match
		// try prev
	}
	return bl;
}

/// Returns true if the current block_list exists in the database.
///
/// @param mapit Iterator
/// @return true if it exists
bool mapit_exists(struct s_mapiterator* mapit)
{
	nullpo_retr(false,mapit);

	return dbi_exists(mapit->dbi);
}

/*==========================================
 * map.npcへ追加 (warp等の領域持ちのみ)
 *------------------------------------------*/
bool map_addnpc(int m,struct npc_data *nd)
{
	nullpo_retr(0, nd);

	if( m < 0 || m >= map_num )
		return false;

	if( map[m].npc_num == MAX_NPC_PER_MAP )
	{
		ShowWarning("too many NPCs in one map %s\n",map[m].name);
		return false;
	}

	map[m].npc[map[m].npc_num]=nd;
	map[m].npc_num++;
	idb_put(id_db,nd->bl.id,nd);
	return true;
}

/*=========================================
 * Dynamic Mobs [Wizputer]
 *-----------------------------------------*/
// Stores the spawn data entry in the mob list.
// Returns the index of successful, or -1 if the list was full.
int map_addmobtolist(unsigned short m, struct spawn_data *spawn)
{
	size_t i;
	ARR_FIND( 0, MAX_MOB_LIST_PER_MAP, i, map[m].moblist[i] == NULL );
	if( i < MAX_MOB_LIST_PER_MAP )
	{
		map[m].moblist[i] = spawn;
		return i;
	}
	return -1;
}

void map_spawnmobs(int m)
{
	int i, k=0;
	if (map[m].mob_delete_timer != -1)
	{	//Mobs have not been removed yet [Skotlex]
		delete_timer(map[m].mob_delete_timer, map_removemobs_timer);
		map[m].mob_delete_timer = INVALID_TIMER;
		return;
	}
	for(i=0; i<MAX_MOB_LIST_PER_MAP; i++)
		if(map[m].moblist[i]!=NULL)
		{
			k+=map[m].moblist[i]->num;
			npc_parse_mob2(map[m].moblist[i]);
		}

	if (battle_config.etc_log && k > 0)
	{
		ShowStatus("Map %s: Spawned '"CL_WHITE"%d"CL_RESET"' mobs.\n",map[m].name, k);
	}
}

int map_removemobs_sub(struct block_list *bl, va_list ap)
{
	struct mob_data *md = (struct mob_data *)bl;
	nullpo_retr(0, md);

	//When not to remove mob:
	// doesn't respawn and is not a slave
	if( !md->spawn && !md->master_id )
		return 0;
	// respawn data is not in cache
	if( md->spawn && !md->spawn->state.dynamic )
		return 0;
	// hasn't spawned yet
	if( md->spawn_timer != INVALID_TIMER )
		return 0;
	// is damaged and mob_remove_damaged is off
	if( !battle_config.mob_remove_damaged && md->status.hp < md->status.max_hp )
		return 0;
	// is a mvp
	if( md->db->mexp > 0 )
		return 0;
	
	unit_free(&md->bl,0);

	return 1;
}

int map_removemobs_timer(int tid, unsigned int tick, int id, intptr data)
{
	int count;
	const int m = id;

	if (m < 0 || m >= MAX_MAP_PER_SERVER)
	{	//Incorrect map id!
		ShowError("map_removemobs_timer error: timer %d points to invalid map %d\n",tid, m);
		return 0;
	}
	if (map[m].mob_delete_timer != tid)
	{	//Incorrect timer call!
		ShowError("map_removemobs_timer mismatch: %d != %d (map %s)\n",map[m].mob_delete_timer, tid, map[m].name);
		return 0;
	}
	map[m].mob_delete_timer = INVALID_TIMER;
	if (map[m].users > 0) //Map not empty!
		return 1;

	count = map_foreachinmap(map_removemobs_sub, m, BL_MOB);

	if (battle_config.etc_log && count > 0)
		ShowStatus("Map %s: Removed '"CL_WHITE"%d"CL_RESET"' mobs.\n",map[m].name, count);
	
	return 1;
}

void map_removemobs(int m)
{
	if (map[m].mob_delete_timer != -1) // should never happen
		return; //Mobs are already scheduled for removal

	map[m].mob_delete_timer = add_timer(gettick()+battle_config.mob_remove_delay, map_removemobs_timer, m, 0);
}

/*==========================================
 * map名からmap番?へ?換
 *------------------------------------------*/
int map_mapname2mapid(const char* name)
{
	unsigned short map_index;
	map_index = mapindex_name2id(name);
	if (!map_index)
		return -1;
	return map_mapindex2mapid(map_index);
}

/*==========================================
 * Returns the map of the given mapindex. [Skotlex]
 *------------------------------------------*/
int map_mapindex2mapid(unsigned short mapindex)
{
	struct map_data *md=NULL;
	
	if (!mapindex)
		return -1;
	
	md = (struct map_data*)uidb_get(map_db,(unsigned int)mapindex);
	if(md==NULL || md->cell==NULL)
		return -1;
	return md->m;
}

/*==========================================
 * 他鯖map名からip,port?換
 *------------------------------------------*/
int map_mapname2ipport(unsigned short name, uint32* ip, uint16* port)
{
	struct map_data_other_server *mdos=NULL;

	mdos = (struct map_data_other_server*)uidb_get(map_db,(unsigned int)name);
	if(mdos==NULL || mdos->cell) //If gat isn't null, this is a local map.
		return -1;
	*ip=mdos->ip;
	*port=mdos->port;
	return 0;
}

/*==========================================
 * Checks if both dirs point in the same direction.
 *------------------------------------------*/
int map_check_dir(int s_dir,int t_dir)
{
	if(s_dir == t_dir)
		return 0;
	switch(s_dir) {
		case 0: if(t_dir == 7 || t_dir == 1 || t_dir == 0) return 0; break;
		case 1: if(t_dir == 0 || t_dir == 2 || t_dir == 1) return 0; break;
		case 2: if(t_dir == 1 || t_dir == 3 || t_dir == 2) return 0; break;
		case 3: if(t_dir == 2 || t_dir == 4 || t_dir == 3) return 0; break;
		case 4: if(t_dir == 3 || t_dir == 5 || t_dir == 4) return 0; break;
		case 5: if(t_dir == 4 || t_dir == 6 || t_dir == 5) return 0; break;
		case 6: if(t_dir == 5 || t_dir == 7 || t_dir == 6) return 0; break;
		case 7: if(t_dir == 6 || t_dir == 0 || t_dir == 7) return 0; break;
	}
	return 1;
}

/*==========================================
 * Returns the direction of the given cell, relative to 'src'
 *------------------------------------------*/
uint8 map_calc_dir(struct block_list* src, int x, int y)
{
	unsigned char dir = 0;
	int dx, dy;
	
	nullpo_retr(0, src);
	
	dx = x-src->x;
	dy = y-src->y;
	if( dx == 0 && dy == 0 )
	{	// both are standing on the same spot
		//dir = 6; // aegis-style, makes knockback default to the left
		dir = unit_getdir(src); // athena-style, makes knockback default to behind 'src'
	}
	else if( dx >= 0 && dy >=0 )
	{	// upper-right
		if( dx*2 <= dy )      dir = 0;	// up
		else if( dx > dy*2 )  dir = 6;	// right
		else                  dir = 7;	// up-right
	}
	else if( dx >= 0 && dy <= 0 )
	{	// lower-right
		if( dx*2 <= -dy )     dir = 4;	// down
		else if( dx > -dy*2 ) dir = 6;	// right
		else                  dir = 5;	// down-right
	}
	else if( dx <= 0 && dy <= 0 )
	{	// lower-left
		if( dx*2 >= dy )      dir = 4;	// down
		else if( dx < dy*2 )  dir = 2;	// left
		else                  dir = 3;	// down-left
	}
	else
	{	// upper-left
		if( -dx*2 <= dy )     dir = 0;	// up
		else if( -dx > dy*2 ) dir = 2;	// left
		else                  dir = 1;	// up-left
	
	}
	return dir;
}

/*==========================================
 * Randomizes target cell x,y to a random walkable cell that 
 * has the same distance from object as given coordinates do. [Skotlex]
 *------------------------------------------*/
int map_random_dir(struct block_list *bl, short *x, short *y)
{
	short xi = *x-bl->x;
	short yi = *y-bl->y;
	short i=0, j;
	int dist2 = xi*xi + yi*yi;
	short dist = (short)sqrt((float)dist2);
	short segment;
	
	if (dist < 1) dist =1;
	
	do {
		j = rand()%8; //Pick a random direction
		segment = 1+(rand()%dist); //Pick a random interval from the whole vector in that direction
		xi = bl->x + segment*dirx[j];
		segment = (short)sqrt((float)(dist2 - segment*segment)); //The complement of the previously picked segment
		yi = bl->y + segment*diry[j];
	} while (
		(map_getcell(bl->m,xi,yi,CELL_CHKNOPASS) || !path_search(NULL,bl->m,bl->x,bl->y,xi,yi,1,CELL_CHKNOREACH))
		&& (++i)<100 );
	
	if (i < 100) {
		*x = xi;
		*y = yi;
		return 1;
	}
	return 0;
}

// gat系
static struct mapcell map_gat2cell(int gat)
{
	struct mapcell cell;
	memset(&cell, 0, sizeof(cell));

	switch( gat )
	{
	case 0: cell.walkable = 1; cell.shootable = 1; cell.water = 0; break; // walkable ground
	case 1: cell.walkable = 0; cell.shootable = 0; cell.water = 0; break; // non-walkable ground
	case 2: cell.walkable = 1; cell.shootable = 1; cell.water = 0; break; // ???
	case 3: cell.walkable = 1; cell.shootable = 1; cell.water = 1; break; // walkable water
	case 4: cell.walkable = 1; cell.shootable = 1; cell.water = 0; break; // ???
	case 5: cell.walkable = 0; cell.shootable = 1; cell.water = 0; break; // gap (snipable)
	case 6: cell.walkable = 1; cell.shootable = 1; cell.water = 0; break; // ???
	default:
		ShowWarning("map_gat2cell: unrecognized gat type '%d'\n", gat);
		break;
	}

	return cell;
}

static int map_cell2gat(struct mapcell cell)
{
	if( cell.walkable == 1 && cell.shootable == 1 && cell.water == 0 ) return 0;
	if( cell.walkable == 0 && cell.shootable == 0 && cell.water == 0 ) return 1;
	if( cell.walkable == 1 && cell.shootable == 1 && cell.water == 1 ) return 3;
	if( cell.walkable == 0 && cell.shootable == 1 && cell.water == 0 ) return 5;

	ShowWarning("map_cell2gat: cell has no matching gat type\n");
	return 1; // default to 'wall'
}

/*==========================================
 * (m,x,y)の状態を調べる
 *------------------------------------------*/
int map_getcell(int m,int x,int y,cell_chk cellchk)
{
	return (m < 0 || m >= MAX_MAP_PER_SERVER) ? 0 : map_getcellp(&map[m],x,y,cellchk);
}

int map_getcellp(struct map_data* m,int x,int y,cell_chk cellchk)
{
	struct mapcell cell;

	nullpo_ret(m);

	//NOTE: this intentionally overrides the last row and column
	if(x<0 || x>=m->xs-1 || y<0 || y>=m->ys-1)
		return( cellchk == CELL_CHKNOPASS );

	cell = m->cell[x + y*m->xs];

	switch(cellchk)
	{
		// gat type retrieval
		case CELL_GETTYPE:
			return map_cell2gat(cell);

		// base gat type checks
		case CELL_CHKWALL:
			return (!cell.walkable && !cell.shootable);
			//return (map_cell2gat(cell) == 1);
		case CELL_CHKWATER:
			return (cell.water);
			//return (map_cell2gat(cell) == 3);
		case CELL_CHKCLIFF:
			return (!cell.walkable && cell.shootable);
			//return (map_cell2gat(cell) == 5);

		// base cell type checks
		case CELL_CHKNPC:
			return (cell.npc);
		case CELL_CHKBASILICA:
			return (cell.basilica);
		case CELL_CHKLANDPROTECTOR:
			return (cell.landprotector);
		case CELL_CHKNOVENDING:
			return (cell.novending);
		case CELL_CHKNOCHAT:
			return (cell.nochat);

		// special checks
		case CELL_CHKPASS:
#ifdef CELL_NOSTACK
			if (cell.cell_bl >= battle_config.cell_stack_limit) return 0;
#endif
		case CELL_CHKREACH:
			return (cell.walkable);

		case CELL_CHKNOPASS:
#ifdef CELL_NOSTACK
			if (cell.cell_bl >= battle_config.cell_stack_limit) return 1;
#endif
		case CELL_CHKNOREACH:
			return (!cell.walkable);

		case CELL_CHKSTACK:
#ifdef CELL_NOSTACK
			return (cell.cell_bl >= battle_config.cell_stack_limit);
#else
			return 0;
#endif

		default:
			return 0;
	}
}

/*==========================================
 * Change the type/flags of a map cell
 * 'cell' - which flag to modify
 * 'flag' - true = on, false = off
 *------------------------------------------*/
void map_setcell(int m, int x, int y, cell_t cell, bool flag)
{
	int j;

	if( m < 0 || m >= map_num || x < 0 || x >= map[m].xs || y < 0 || y >= map[m].ys )
		return;

	j = x + y*map[m].xs;

	switch( cell ) {
		case CELL_WALKABLE:      map[m].cell[j].walkable = flag;      break;
		case CELL_SHOOTABLE:     map[m].cell[j].shootable = flag;     break;
		case CELL_WATER:         map[m].cell[j].water = flag;         break;

		case CELL_NPC:           map[m].cell[j].npc = flag;           break;
		case CELL_BASILICA:      map[m].cell[j].basilica = flag;      break;
		case CELL_LANDPROTECTOR: map[m].cell[j].landprotector = flag; break;
		case CELL_NOVENDING:     map[m].cell[j].novending = flag;     break;
		case CELL_NOCHAT:        map[m].cell[j].nochat = flag;        break;
		default:
			ShowWarning("map_setcell: invalid cell type '%d'\n", (int)cell);
			break;
	}
}

void map_setgatcell(int m, int x, int y, int gat)
{
	int j;
	struct mapcell cell;

	if( m < 0 || m >= map_num || x < 0 || x >= map[m].xs || y < 0 || y >= map[m].ys )
		return;

	j = x + y*map[m].xs;

	cell = map_gat2cell(gat);
	map[m].cell[j].walkable = cell.walkable;
	map[m].cell[j].shootable = cell.shootable;
	map[m].cell[j].water = cell.water;
}

/*==========================================
 * Invisible Walls
 *------------------------------------------*/
static DBMap* iwall_db;

void map_iwall_nextxy(int x, int y, int dir, int pos, int *x1, int *y1)
{
	if( dir == 0 || dir == 4 )
		*x1 = x; // Keep X
	else if( dir > 0 && dir < 4 )
		*x1 = x - pos; // Going left
	else
		*x1 = x + pos; // Going right

	if( dir == 2 || dir == 6 )
		*y1 = y;
	else if( dir > 2 && dir < 6 )
		*y1 = y - pos;
	else
		*y1 = y + pos;
}

bool map_iwall_set(int m, int x, int y, int size, int dir, bool shootable, const char* wall_name)
{
	struct iwall_data *iwall;
	int i, x1 = 0, y1 = 0;

	if( size < 1 || !wall_name )
		return false;

	if( (iwall = (struct iwall_data *)strdb_get(iwall_db, wall_name)) != NULL )
		return false; // Already Exists

	if( map_getcell(m, x, y, CELL_CHKNOREACH) )
		return false; // Starting cell problem

	CREATE(iwall, struct iwall_data, 1);
	iwall->m = m;
	iwall->x = x;
	iwall->y = y;
	iwall->size = size;
	iwall->dir = dir;
	iwall->shootable = shootable;
	safestrncpy(iwall->wall_name, wall_name, sizeof(iwall->wall_name));

	for( i = 0; i < size; i++ )
	{
		map_iwall_nextxy(x, y, dir, i, &x1, &y1);

		if( map_getcell(m, x1, y1, CELL_CHKNOREACH) )
			break; // Collision

		map_setcell(m, x1, y1, CELL_WALKABLE, false);
		map_setcell(m, x1, y1, CELL_SHOOTABLE, shootable);

		clif_changemapcell(0, m, x1, y1, map_getcell(m, x1, y1, CELL_GETTYPE), ALL_SAMEMAP);
	}

	iwall->size = i;

	strdb_put(iwall_db, iwall->wall_name, iwall);
	map[m].iwall_num++;

	return true;
}

void map_iwall_get(struct map_session_data *sd)
{
	struct iwall_data *iwall;
	DBIterator* iter;
	DBKey key;
	int x1, y1;
	int i;

	if( map[sd->bl.m].iwall_num < 1 )
		return;

	iter = iwall_db->iterator(iwall_db);
	for( iwall = (struct iwall_data *)iter->first(iter,&key); iter->exists(iter); iwall = (struct iwall_data *)iter->next(iter,&key) )
	{
		if( iwall->m != sd->bl.m )
			continue;

		for( i = 0; i < iwall->size; i++ )
		{
			map_iwall_nextxy(iwall->x, iwall->y, iwall->dir, i, &x1, &y1);
			clif_changemapcell(sd->fd, iwall->m, x1, y1, map_getcell(iwall->m, x1, y1, CELL_GETTYPE), SELF);
		}
	}
	iter->destroy(iter);
}

void map_iwall_remove(const char *wall_name)
{
	struct iwall_data *iwall;
	int i, x1, y1;

	if( (iwall = (struct iwall_data *)strdb_get(iwall_db, wall_name)) == NULL )
		return; // Nothing to do

	for( i = 0; i < iwall->size; i++ )
	{
		map_iwall_nextxy(iwall->x, iwall->y, iwall->dir, i, &x1, &y1);

		map_setcell(iwall->m, x1, y1, CELL_SHOOTABLE, true);
		map_setcell(iwall->m, x1, y1, CELL_WALKABLE, true);

		clif_changemapcell(0, iwall->m, x1, y1, map_getcell(iwall->m, x1, y1, CELL_GETTYPE), ALL_SAMEMAP);
	}

	map[iwall->m].iwall_num--;
	strdb_remove(iwall_db, iwall->wall_name);
}

static void* create_map_data_other_server(DBKey key, va_list args)
{
	struct map_data_other_server *mdos;
	unsigned short mapindex = (unsigned short)key.ui;
	mdos=(struct map_data_other_server *)aCalloc(1,sizeof(struct map_data_other_server));
	mdos->index = mapindex;
	memcpy(mdos->name, mapindex_id2name(mapindex), MAP_NAME_LENGTH);
	return mdos;
}

/*==========================================
 * 他鯖管理のマップをdbに追加
 *------------------------------------------*/
int map_setipport(unsigned short mapindex, uint32 ip, uint16 port)
{
	struct map_data_other_server *mdos=NULL;

	mdos=(struct map_data_other_server *)uidb_ensure(map_db,(unsigned int)mapindex, create_map_data_other_server);
	
	if(mdos->cell) //Local map,Do nothing. Give priority to our own local maps over ones from another server. [Skotlex]
		return 0;
	if(ip == clif_getip() && port == clif_getport()) {
		//That's odd, we received info that we are the ones with this map, but... we don't have it.
		ShowFatalError("map_setipport : received info that this map-server SHOULD have map '%s', but it is not loaded.\n",mapindex_id2name(mapindex));
		exit(EXIT_FAILURE);
	}
	mdos->ip   = ip;
	mdos->port = port;
	return 1;
}

/*==========================================
 * 他鯖管理のマップを全て削除
 *------------------------------------------*/
int map_eraseallipport_sub(DBKey key,void *data,va_list va)
{
	struct map_data_other_server *mdos = (struct map_data_other_server*)data;
	if(mdos->cell == NULL) {
		db_remove(map_db,key);
		aFree(mdos);
	}
	return 0;
}

int map_eraseallipport(void)
{
	map_db->foreach(map_db,map_eraseallipport_sub);
	return 1;
}

/*==========================================
 * 他鯖管理のマップをdbから削除
 *------------------------------------------*/
int map_eraseipport(unsigned short mapindex, uint32 ip, uint16 port)
{
	struct map_data_other_server *mdos;

	mdos = (struct map_data_other_server*)uidb_get(map_db,(unsigned int)mapindex);
	if(!mdos || mdos->cell) //Map either does not exists or is a local map.
		return 0;

	if(mdos->ip==ip && mdos->port == port) {
		uidb_remove(map_db,(unsigned int)mapindex);
		aFree(mdos);
		return 1;
	}
	return 0;
}

/*==========================================
 * Map cache reading
 *==========================================*/
int map_readfromcache(struct map_data *m, FILE *fp)
{
	struct map_cache_main_header header;
	struct map_cache_map_info info;
	int i;
	
	if( !fp )
		return 0;

	fseek(fp, 0, SEEK_SET);
	fread(&header, sizeof(struct map_cache_main_header), 1, fp);

	for( i = 0; i < header.map_count; ++i )
	{
		fread(&info, sizeof(struct map_cache_map_info), 1, fp);

		if( strcmp(m->name, info.name) == 0 )
			break; // Map found

		// Map not found, jump to the beginning of the next map info header
		fseek(fp, info.len, SEEK_CUR);
	}

	if( i < header.map_count )
	{
		unsigned char *buf, *buf2;
		unsigned long size, xy;

		if( info.xs <= 0 || info.ys <= 0 )
			return 0;// invalid

		m->xs = info.xs;
		m->ys = info.ys;
		size = (unsigned long)info.xs*(unsigned long)info.ys;

		buf = (unsigned char*)aMalloc(info.len); // temp buffer to read the zipped map
		buf2 = (unsigned char*)aMalloc(size); // temp buffer to unpack the data
		CREATE(m->cell, struct mapcell, size);

		fread(buf, info.len, 1, fp);
		decode_zip(buf2, &size, buf, info.len); // Unzip the map from the buffer

		for( xy = 0; xy < size; ++xy )
			m->cell[xy] = map_gat2cell(buf2[xy]);

		aFree(buf);
		aFree(buf2);
		return 1;
	}

	return 0;// not found
}

int map_addmap(char* mapname)
{
	if( strcmpi(mapname,"clear")==0 )
	{
		map_num = 0;
		instance_start = 0;
		return 0;
	}

	if( map_num >= MAX_MAP_PER_SERVER - 1 )
	{
		ShowError("Could not add map '"CL_WHITE"%s"CL_RESET"', the limit of maps has been reached.\n",mapname);
		return 1;
	}

	mapindex_getmapname(mapname, map[map_num].name);
	map_num++;
	return 0;
}

static void map_delmapid(int id)
{
	ShowNotice("Removing map [ %s ] from maplist"CL_CLL"\n",map[id].name);
	memmove(map+id, map+id+1, sizeof(map[0])*(map_num-id-1));
	map_num--;
}

int map_delmap(char* mapname)
{
	int i;
	char map_name[MAP_NAME_LENGTH];

	if (strcmpi(mapname, "all") == 0) {
		map_num = 0;
		return 0;
	}

	mapindex_getmapname(mapname, map_name);
	for(i = 0; i < map_num; i++) {
		if (strcmp(map[i].name, map_name) == 0) {
			map_delmapid(i);
			return 1;
		}
	}
	return 0;
}

#define NO_WATER 1000000

/*
 * Reads from the .rsw for each map
 * Returns water height (or NO_WATER if file doesn't exist) or other error is encountered.
 * Assumed path for file is data/mapname.rsw
 * Credits to LittleWolf
 */
int map_waterheight(char* mapname)
{
	char fn[256];
 	char *rsw, *found;

	//Look up for the rsw
	sprintf(fn, "data\\%s.rsw", mapname);

	found = grfio_find_file(fn);
	if (found) strcpy(fn, found); // replace with real name
	
	// read & convert fn
	rsw = (char *) grfio_read (fn);
	if (rsw)
	{	//Load water height from file
		int wh = (int) *(float*)(rsw+166);
		aFree(rsw);
		return wh;
	}
	ShowWarning("Failed to find water level for (%s)\n", mapname, fn);
	return NO_WATER;
}

/*==================================
 * .GAT format
 *----------------------------------*/
int map_readgat (struct map_data* m)
{
	char filename[256];
	uint8* gat;
	int water_height;
	size_t xy, off, num_cells;

	sprintf(filename, "data\\%s.gat", m->name);

	gat = (uint8 *) grfio_read(filename);
	if (gat == NULL)
		return 0;

	m->xs = *(int32*)(gat+6);
	m->ys = *(int32*)(gat+10);
	num_cells = m->xs * m->ys;
	CREATE(m->cell, struct mapcell, num_cells);

	water_height = map_waterheight(m->name);

	// Set cell properties
	off = 14;
	for( xy = 0; xy < num_cells; ++xy )
	{
		// read cell data
		float height = *(float*)( gat + off      );
		uint32 type = *(uint32*)( gat + off + 16 );
		off += 20;

		if( type == 0 && water_height != NO_WATER && height > water_height )
			type = 3; // Cell is 0 (walkable) but under water level, set to 3 (walkable water)

		m->cell[xy] = map_gat2cell(type);
	}
	
	aFree(gat);

	return 1;
}

/*======================================
 * Add/Remove map to the map_db
 *--------------------------------------*/
void map_addmap2db(struct map_data *m)
{
	uidb_put(map_db, (unsigned int)m->index, m);
}

void map_removemapdb(struct map_data *m)
{
	uidb_remove(map_db, (unsigned int)m->index);
}

/*======================================
 * Initiate maps loading stage
 *--------------------------------------*/
int map_readallmaps (void)
{
	int i;
	FILE* fp=NULL;
	int maps_removed = 0;

	if( enable_grf )
		ShowStatus("Loading maps (using GRF files)...\n");
	else
	{
		ShowStatus("Loading maps (using %s as map cache)...\n", map_cache_file);
		if( (fp = fopen(map_cache_file, "rb")) == NULL )
		{
			ShowFatalError("Unable to open map cache file "CL_WHITE"%s"CL_RESET"\n", map_cache_file);
			exit(EXIT_FAILURE); //No use launching server if maps can't be read.
		}
	}

	for(i = 0; i < map_num; i++)
	{
		size_t size;

		// show progress
		ShowStatus("Loading maps [%i/%i]: %s"CL_CLL"\r", i, map_num, map[i].name);

		// try to load the map
		if( !
			(enable_grf?
				 map_readgat(&map[i])
				:map_readfromcache(&map[i], fp))
			) {
			map_delmapid(i);
			maps_removed++;
			i--;
			continue;
		}

		map[i].index = mapindex_name2id(map[i].name);

		if (uidb_get(map_db,(unsigned int)map[i].index) != NULL)
		{
			ShowWarning("Map %s already loaded!"CL_CLL"\n", map[i].name);
			if (map[i].cell) {
				aFree(map[i].cell);
				map[i].cell = NULL;	
			}
			map_delmapid(i);
			maps_removed++;
			i--;
			continue;
		}

		map_addmap2db(&map[i]);

		map[i].m = i;
		memset(map[i].moblist, 0, sizeof(map[i].moblist));	//Initialize moblist [Skotlex]
		map[i].mob_delete_timer = INVALID_TIMER;	//Initialize timer [Skotlex]
		if(battle_config.pk_mode)
			map[i].flag.pvp = 1; // make all maps pvp for pk_mode [Valaris]

		map[i].bxs = (map[i].xs + BLOCK_SIZE - 1) / BLOCK_SIZE;
		map[i].bys = (map[i].ys + BLOCK_SIZE - 1) / BLOCK_SIZE;
		
		// default experience multiplicators
		map[i].jexp = 100;
		map[i].bexp = 100;
		
		size = map[i].bxs * map[i].bys * sizeof(struct block_list*);
		map[i].block = (struct block_list**)aCalloc(size, 1);
		map[i].block_mob = (struct block_list**)aCalloc(size, 1);
	}

	if( !enable_grf )
		fclose(fp);

	// finished map loading
	ShowInfo("Successfully loaded '"CL_WHITE"%d"CL_RESET"' maps."CL_CLL"\n",map_num);
	instance_start = map_num; // Next Map Index will be instances

	if (maps_removed)
		ShowNotice("Maps removed: '"CL_WHITE"%d"CL_RESET"'\n",maps_removed);

	return 0;
}

////////////////////////////////////////////////////////////////////////
static int map_ip_set = 0;
static int char_ip_set = 0;

/*==========================================
 * Console Command Parser [Wizputer]
 *------------------------------------------*/
int parse_console(char* buf)
{
	char type[64];
	char command[64];
	char map[64];
	int x = 0;
	int y = 0;
	int m;
	int n;
	struct map_session_data sd;

	memset(&sd, 0, sizeof(struct map_session_data));
	strcpy(sd.status.name, "console");

	if( (n=sscanf(buf, "%[^:]:%[^:]:%99s %d %d[^\n]",type,command,map,&x,&y)) < 5 )
		if( (n=sscanf(buf, "%[^:]:%[^\n]",type,command)) < 2 )
			n = sscanf(buf,"%[^\n]",type);

	if( n == 5 ) {
		m = map_mapname2mapid(map);
		if( m < 0 ){
			ShowWarning("Console: Unknown map\n");
			return 0;
		}
		sd.bl.m = m;
		map_search_freecell(&sd.bl, m, &sd.bl.x, &sd.bl.y, -1, -1, 0); 
		if( x > 0 )
			sd.bl.x = x;
		if( y > 0 )
			sd.bl.y = y;
	} else {
		map[0] = '\0';
		if( n < 2 ) command[0] = '\0';
		if( n < 1 ) type[0] = '\0';
	}

	ShowInfo("Type of command: '%s' || Command: '%s' || Map: '%s' Coords: %d %d\n", type, command, map, x, y);

	if( n == 5 && strcmpi("admin",type) == 0 ){
		if( !is_atcommand(sd.fd,&sd,command,0) )
			ShowInfo("Console: not atcommand\n");
	} else if( n == 2 && strcmpi("server",type) == 0 ){
		if( strcmpi("shutdown",command) == 0 ||
		    strcmpi("exit",command) == 0 ||
		    strcmpi("quit",command) == 0 ){
			runflag = 0;
		}
	} else if( strcmpi("help",type) == 0 ){
		ShowNotice("To use GM commands:\n");
		ShowInfo("admin:<gm command>:<map of \"gm\"> <x> <y>\n");
		ShowInfo("You can use any GM command that doesn't require the GM.\n");
		ShowInfo("No using @item or @warp however you can use @charwarp\n");
		ShowInfo("The <map of \"gm\"> <x> <y> is for commands that need coords of the GM\n");
		ShowInfo("IE: @spawn\n");
		ShowInfo("To shutdown the server:\n");
		ShowInfo("server:shutdown\n");
	}

	return 0;
}

/*==========================================
 * 設定ファイルを?み?む
 *------------------------------------------*/
int map_config_read(char *cfgName)
{
	char line[1024], w1[1024], w2[1024];
	FILE *fp;

	fp = fopen(cfgName,"r");
	if( fp == NULL )
	{
		ShowError("Map configuration file not found at: %s\n", cfgName);
		return 1;
	}

	while( fgets(line, sizeof(line), fp) )
	{
		char* ptr;

		if( line[0] == '/' && line[1] == '/' )
			continue;
		if( (ptr = strstr(line, "//")) != NULL )
			*ptr = '\n'; //Strip comments
		if( sscanf(line, "%[^:]: %[^\t\r\n]", w1, w2) < 2 )
			continue;

		//Strip trailing spaces
		ptr = w2 + strlen(w2);
		while (--ptr >= w2 && *ptr == ' ');
		ptr++;
		*ptr = '\0';
			
		if(strcmpi(w1,"timestamp_format")==0)
			strncpy(timestamp_format, w2, 20);
		else 
		if(strcmpi(w1,"stdout_with_ansisequence")==0)
			stdout_with_ansisequence = config_switch(w2);
		else
		if(strcmpi(w1,"console_silent")==0) {
			ShowInfo("Console Silent Setting: %d\n", atoi(w2));
			msg_silent = atoi(w2);
		} else
		if (strcmpi(w1, "userid")==0)
			chrif_setuserid(w2);
		else
		if (strcmpi(w1, "passwd") == 0)
			chrif_setpasswd(w2);
		else
		if (strcmpi(w1, "char_ip") == 0)
			char_ip_set = chrif_setip(w2);
		else
		if (strcmpi(w1, "char_port") == 0)
			chrif_setport(atoi(w2));
		else
		if (strcmpi(w1, "map_ip") == 0)
			map_ip_set = clif_setip(w2);
		else
		if (strcmpi(w1, "bind_ip") == 0)
			clif_setbindip(w2);
		else
		if (strcmpi(w1, "map_port") == 0) {
			clif_setport(atoi(w2));
			map_port = (atoi(w2));
		} else
		if (strcmpi(w1, "map") == 0)
			map_addmap(w2);
		else
		if (strcmpi(w1, "delmap") == 0)
			map_delmap(w2);
		else
		if (strcmpi(w1, "npc") == 0)
			npc_addsrcfile(w2);
		else
		if (strcmpi(w1, "delnpc") == 0)
			npc_delsrcfile(w2);
		else if (strcmpi(w1, "autosave_time") == 0) {
			autosave_interval = atoi(w2);
			if (autosave_interval < 1) //Revert to default saving.
				autosave_interval = DEFAULT_AUTOSAVE_INTERVAL;
			else
				autosave_interval *= 1000; //Pass from sec to ms
		} else
		if (strcmpi(w1, "minsave_time") == 0) {
			minsave_interval= atoi(w2);
			if (minsave_interval < 1)
				minsave_interval = 1;
		} else
		if (strcmpi(w1, "save_settings") == 0)
			save_settings = atoi(w2);
		else
		if (strcmpi(w1, "motd_txt") == 0)
			strcpy(motd_txt, w2);
		else
		if (strcmpi(w1, "help_txt") == 0)
			strcpy(help_txt, w2);
		else
		if (strcmpi(w1, "help2_txt") == 0)
			strcpy(help2_txt, w2);
		else
		if (strcmpi(w1, "charhelp_txt") == 0)
			strcpy(charhelp_txt, w2);
		else
		if(strcmpi(w1,"map_cache_file") == 0)
			strncpy(map_cache_file,w2,255);
		else
		if(strcmpi(w1,"db_path") == 0)
			strncpy(db_path,w2,255);
		else
		if (strcmpi(w1, "console") == 0) {
			console = config_switch(w2);
			if (console)
				ShowNotice("Console Commands are enabled.\n");
		} else
		if (strcmpi(w1, "enable_spy") == 0)
			enable_spy = config_switch(w2);
		else
		if (strcmpi(w1, "use_grf") == 0)
			enable_grf = config_switch(w2);
		else
		if (strcmpi(w1, "import") == 0)
			map_config_read(w2);
		else
			ShowWarning("Unknown setting '%s' in file %s\n", w1, cfgName);
	}

	fclose(fp);
	return 0;
}

int inter_config_read(char *cfgName)
{
	char line[1024],w1[1024],w2[1024];
	FILE *fp;

	fp=fopen(cfgName,"r");
	if(fp==NULL){
		ShowError("File not found: '%s'.\n",cfgName);
		return 1;
	}
	while(fgets(line, sizeof(line), fp))
	{
		if(line[0] == '/' && line[1] == '/')
			continue;
		if( sscanf(line,"%[^:]: %[^\r\n]",w1,w2) < 2 )
			continue;

		if(strcmpi(w1, "main_chat_nick")==0)
			strcpy(main_chat_nick, w2);
			
	#ifndef TXT_ONLY
		else
		if(strcmpi(w1,"item_db_db")==0)
			strcpy(item_db_db,w2);
		else
		if(strcmpi(w1,"mob_db_db")==0)
			strcpy(mob_db_db,w2);
		else
		if(strcmpi(w1,"item_db2_db")==0)
			strcpy(item_db2_db,w2);
		else
		if(strcmpi(w1,"mob_db2_db")==0)
			strcpy(mob_db2_db,w2);
		else
		//Map Server SQL DB
		if(strcmpi(w1,"map_server_ip")==0)
			strcpy(map_server_ip, w2);
		else
		if(strcmpi(w1,"map_server_port")==0)
			map_server_port=atoi(w2);
		else
		if(strcmpi(w1,"map_server_id")==0)
			strcpy(map_server_id, w2);
		else
		if(strcmpi(w1,"map_server_pw")==0)
			strcpy(map_server_pw, w2);
		else
		if(strcmpi(w1,"map_server_db")==0)
			strcpy(map_server_db, w2);
		else
		if(strcmpi(w1,"default_codepage")==0)
			strcpy(default_codepage, w2);
		else
		if(strcmpi(w1,"use_sql_db")==0) {
			db_use_sqldbs = config_switch(w2);
			ShowStatus ("Using SQL dbs: %s\n",w2);
		} else
		if(strcmpi(w1,"log_db_ip")==0)
			strcpy(log_db_ip, w2);
		else
		if(strcmpi(w1,"log_db_id")==0)
			strcpy(log_db_id, w2);
		else
		if(strcmpi(w1,"log_db_pw")==0)
			strcpy(log_db_pw, w2);
		else
		if(strcmpi(w1,"log_db_port")==0)
			log_db_port = atoi(w2);
		else
		if(strcmpi(w1,"log_db_db")==0)
			strcpy(log_db_db, w2);
	#endif
		else
		if( mapreg_config_read(w1,w2) )
			continue;
		//support the import command, just like any other config
		else
		if(strcmpi(w1,"import")==0)
			inter_config_read(w2);
	}
	fclose(fp);

	return 0;
}

#ifndef TXT_ONLY
/*=======================================
 *  MySQL Init
 *---------------------------------------*/
int map_sql_init(void)
{
	// main db connection
	mmysql_handle = Sql_Malloc();

	ShowInfo("Connecting to the Map DB Server....\n");
	if( SQL_ERROR == Sql_Connect(mmysql_handle, map_server_id, map_server_pw, map_server_ip, map_server_port, map_server_db) )
		exit(EXIT_FAILURE);
	ShowStatus("connect success! (Map Server Connection)\n");

	if( strlen(default_codepage) > 0 )
		if ( SQL_ERROR == Sql_SetEncoding(mmysql_handle, default_codepage) )
			Sql_ShowDebug(mmysql_handle);

	return 0;
}

int map_sql_close(void)
{
	ShowStatus("Close Map DB Connection....\n");
	Sql_Free(mmysql_handle);
	mmysql_handle = NULL;

	if (log_config.sql_logs)
	{
		ShowStatus("Close Log DB Connection....\n");
		Sql_Free(logmysql_handle);
		logmysql_handle = NULL;
	}

	return 0;
}

int log_sql_init(void)
{
	// log db connection
	logmysql_handle = Sql_Malloc();

	ShowInfo(""CL_WHITE"[SQL]"CL_RESET": Connecting to the Log Database "CL_WHITE"%s"CL_RESET" At "CL_WHITE"%s"CL_RESET"...\n",log_db_db,log_db_ip);
	if ( SQL_ERROR == Sql_Connect(logmysql_handle, log_db_id, log_db_pw, log_db_ip, log_db_port, log_db_db) )
		exit(EXIT_FAILURE);
	ShowStatus(""CL_WHITE"[SQL]"CL_RESET": Successfully '"CL_GREEN"connected"CL_RESET"' to Database '"CL_WHITE"%s"CL_RESET"'.\n", log_db_db);

	if( strlen(default_codepage) > 0 )
		if ( SQL_ERROR == Sql_SetEncoding(logmysql_handle, default_codepage) )
			Sql_ShowDebug(logmysql_handle);

	return 0;
}

#endif /* not TXT_ONLY */

int map_db_final(DBKey k,void *d,va_list ap)
{
	struct map_data_other_server *mdos = (struct map_data_other_server*)d;
	if(mdos && mdos->cell == NULL)
		aFree(mdos);
	return 0;
}

int nick_db_final(DBKey key, void *data, va_list args)
{
	struct charid2nick* p = (struct charid2nick*)data;
	struct charid_request* req;

	if( p == NULL )
		return 0;
	while( p->requests )
	{
		req = p->requests;
		p->requests = req->next;
		aFree(req);
	}
	aFree(p);
	return 0;
}

int cleanup_sub(struct block_list *bl, va_list ap)
{
	nullpo_retr(0, bl);

	switch(bl->type) {
		case BL_PC:
			map_quit((struct map_session_data *) bl);
			break;
		case BL_NPC:
			npc_unload((struct npc_data *)bl);
			break;
		case BL_MOB:
			unit_free(bl,0);
			break;
		case BL_PET:
		//There is no need for this, the pet is removed together with the player. [Skotlex]
			break;
		case BL_ITEM:
			map_clearflooritem(bl->id);
			break;
		case BL_SKILL:
			skill_delunit((struct skill_unit *) bl);
			break;
	}

	return 1;
}

static int cleanup_db_sub(DBKey key,void *data,va_list va)
{
	return cleanup_sub((struct block_list*)data, va);
}

/*==========================================
 * map鯖終了・理
 *------------------------------------------*/
void do_final(void)
{
	int i, j;
	struct map_session_data* sd;
	struct s_mapiterator* iter;

	ShowStatus("Terminating...\n");

	for (i = 0; i < map_num; i++)
		if (map[i].m >= 0)
			map_foreachinmap(cleanup_sub, i, BL_ALL);

	//Scan any remaining players (between maps?) to kick them out. [Skotlex]
	iter = mapit_getallusers();
	for( sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC*)mapit_next(iter) )
		map_quit(sd);
	mapit_free(iter);
	
	for( i = 0; i < MAX_INSTANCE; i++ )
		instance_destroy(i);

	id_db->foreach(id_db,cleanup_db_sub);
	chrif_char_reset_offline();
	chrif_flush_fifo();

	do_final_atcommand();
	do_final_battle();
	do_final_chrif();
	do_final_npc();
	do_final_script();
	do_final_itemdb();
	do_final_storage();
	do_final_guild();
	do_final_party();
	do_final_pc();
	do_final_pet();
	do_final_mob();
	do_final_msg();
	do_final_skill();
	do_final_status();
	do_final_unit();
	do_final_battleground();
	
	map_db->destroy(map_db, map_db_final);
	
	for (i=0; i<map_num; i++) {
		if(map[i].cell) aFree(map[i].cell);
		if(map[i].block) aFree(map[i].block);
		if(map[i].block_mob) aFree(map[i].block_mob);
		if(battle_config.dynamic_mobs) { //Dynamic mobs flag by [random]
			for (j=0; j<MAX_MOB_LIST_PER_MAP; j++)
				if (map[i].moblist[j]) aFree(map[i].moblist[j]);
		}
	}

	mapindex_final();
	if(enable_grf)
		grfio_final();

	id_db->destroy(id_db, NULL);
	pc_db->destroy(pc_db, NULL);
	mobid_db->destroy(mobid_db, NULL);
	bossid_db->destroy(bossid_db, NULL);
	nick_db->destroy(nick_db, nick_db_final);
	charid_db->destroy(charid_db, NULL);
	iwall_db->destroy(iwall_db, NULL);
	regen_db->destroy(regen_db, NULL);

#ifndef TXT_ONLY
    map_sql_close();
#endif /* not TXT_ONLY */
	ShowStatus("Successfully terminated.\n");
}

static int map_abort_sub(struct map_session_data* sd, va_list ap)
{
	chrif_save(sd,1);
	return 1;
}


//------------------------------
// Function called when the server
// has received a crash signal.
//------------------------------
void do_abort(void)
{
	static int run = 0;
	//Save all characters and then flush the inter-connection.
	if (run) {
		ShowFatalError("Server has crashed while trying to save characters. Character data can't be saved!\n");
		return;
	}
	run = 1;
	if (!chrif_isconnected())
	{
		if (pc_db->size(pc_db))
			ShowFatalError("Server has crashed without a connection to the char-server, %u characters can't be saved!\n", pc_db->size(pc_db));
		return;
	}
	ShowError("Server received crash signal! Attempting to save all online characters!\n");
	map_foreachpc(map_abort_sub);
	chrif_flush_fifo();
}

/*======================================================
 * Map-Server Version Screen [MC Cameri]
 *------------------------------------------------------*/
void map_helpscreen(int flag)
{
	puts("Usage: map-server [options]");
	puts("Options:");
	puts(CL_WHITE"  Commands\t\t\tDescription"CL_RESET);
	puts("-----------------------------------------------------------------------------");
	puts("  --help, --h, --?, /?		Displays this help screen");
	puts("  --map-config <file>		Load map-server configuration from <file>");
	puts("  --battle-config <file>	Load battle configuration from <file>");
	puts("  --atcommand-config <file>	Load atcommand configuration from <file>");
	puts("  --charcommand-config <file>	Load charcommand configuration from <file>");
	puts("  --script-config <file>	Load script configuration from <file>");
	puts("  --msg-config <file>		Load message configuration from <file>");
	puts("  --grf-path-file <file>	Load grf path file configuration from <file>");
	puts("  --sql-config <file>		Load inter-server configuration from <file>");
	puts("				(SQL Only)");
	puts("  --log-config <file>		Load logging configuration from <file>");
	puts("				(SQL Only)");
	puts("  --version, --v, -v, /v	Displays the server's version");
	puts("\n");
	if (flag) exit(EXIT_FAILURE);
}

/*======================================================
 * Map-Server Version Screen [MC Cameri]
 *------------------------------------------------------*/
void map_versionscreen(int flag)
{
	ShowInfo(CL_WHITE "eAthena version %d.%02d.%02d, Athena Mod version %d" CL_RESET"\n",
		ATHENA_MAJOR_VERSION, ATHENA_MINOR_VERSION, ATHENA_REVISION,
		ATHENA_MOD_VERSION);
	ShowInfo(CL_GREEN "Website/Forum:" CL_RESET "\thttp://eathena.deltaanime.net/\n");
	ShowInfo(CL_GREEN "IRC Channel:" CL_RESET "\tirc://irc.deltaanime.net/#athena\n");
	ShowInfo("\nOpen " CL_WHITE "readme.html" CL_RESET " for more information.");
	if (ATHENA_RELEASE_FLAG) ShowNotice("This version is not for release.\n");
	if (flag) exit(EXIT_FAILURE);
}

/*======================================================
 * Map-Server Init and Command-line Arguments [Valaris]
 *------------------------------------------------------*/
void set_server_type(void)
{
	SERVER_TYPE = ATHENA_SERVER_MAP;
}

int do_init(int argc, char *argv[])
{
	int i;

#ifdef GCOLLECT
	GC_enable_incremental();
#endif

	INTER_CONF_NAME="conf/inter_athena.conf";
	LOG_CONF_NAME="conf/log_athena.conf";
	MAP_CONF_NAME = "conf/map_athena.conf";
	BATTLE_CONF_FILENAME = "conf/battle_athena.conf";
	ATCOMMAND_CONF_FILENAME = "conf/atcommand_athena.conf";
	CHARCOMMAND_CONF_FILENAME = "conf/charcommand_athena.conf";
	SCRIPT_CONF_NAME = "conf/script_athena.conf";
	MSG_CONF_NAME = "conf/msg_athena.conf";
	GRF_PATH_FILENAME = "conf/grf-files.txt";

	srand(gettick());

	for (i = 1; i < argc ; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "--h") == 0 || strcmp(argv[i], "--?") == 0 || strcmp(argv[i], "/?") == 0)
			map_helpscreen(1);
		else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "--v") == 0 || strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "/v") == 0)
			map_versionscreen(1);
		else if (strcmp(argv[i], "--map_config") == 0 || strcmp(argv[i], "--map-config") == 0)
			MAP_CONF_NAME=argv[i+1];
		else if (strcmp(argv[i],"--battle_config") == 0 || strcmp(argv[i],"--battle-config") == 0)
			BATTLE_CONF_FILENAME = argv[i+1];
		else if (strcmp(argv[i],"--atcommand_config") == 0 || strcmp(argv[i],"--atcommand-config") == 0)
			ATCOMMAND_CONF_FILENAME = argv[i+1];
		else if (strcmp(argv[i],"--script_config") == 0 || strcmp(argv[i],"--script-config") == 0)
			SCRIPT_CONF_NAME = argv[i+1];
		else if (strcmp(argv[i],"--msg_config") == 0 || strcmp(argv[i],"--msg-config") == 0)
			MSG_CONF_NAME = argv[i+1];
		else if (strcmp(argv[i],"--grf_path_file") == 0 || strcmp(argv[i],"--grf-path-file") == 0)
			GRF_PATH_FILENAME = argv[i+1];
#ifndef TXT_ONLY
		else if (strcmp(argv[i],"--inter_config") == 0 || strcmp(argv[i],"--inter-config") == 0)
			INTER_CONF_NAME = argv[i+1];
#endif
		else if (strcmp(argv[i],"--log_config") == 0 || strcmp(argv[i],"--log-config") == 0)
			LOG_CONF_NAME = argv[i+1];
		else if (strcmp(argv[i],"--run_once") == 0)	// close the map-server as soon as its done.. for testing [Celest]
			runflag = 0;
	}

	map_config_read(MAP_CONF_NAME);
	chrif_checkdefaultlogin();

	if (!map_ip_set || !char_ip_set) {
		char ip_str[16];
		ip2str(addr_[0], ip_str);

		ShowError("\nNot all IP addresses in map_athena.conf configured, autodetecting...\n");

		if (naddr_ == 0)
			ShowError("Unable to determine your IP address...\n");
		else if (naddr_ > 1)
			ShowNotice("Multiple interfaces detected...\n");

		ShowInfo("Defaulting to %s as our IP address\n", ip_str);

		if (!map_ip_set)
			clif_setip(ip_str);
		if (!char_ip_set)
			chrif_setip(ip_str);
	}

	battle_config_read(BATTLE_CONF_FILENAME);
	msg_config_read(MSG_CONF_NAME);
	atcommand_config_read(ATCOMMAND_CONF_FILENAME);
	script_config_read(SCRIPT_CONF_NAME);
	inter_config_read(INTER_CONF_NAME);
	log_config_read(LOG_CONF_NAME);

	id_db = idb_alloc(DB_OPT_BASE);
	pc_db = idb_alloc(DB_OPT_BASE);	//Added for reliable map_id2sd() use. [Skotlex]
	mobid_db = idb_alloc(DB_OPT_BASE);	//Added to lower the load of the lazy mob ai. [Skotlex]
	bossid_db = idb_alloc(DB_OPT_BASE); // Used for Convex Mirror quick MVP search
	map_db = uidb_alloc(DB_OPT_BASE);
	nick_db = idb_alloc(DB_OPT_BASE);
	charid_db = idb_alloc(DB_OPT_BASE);
	regen_db = idb_alloc(DB_OPT_BASE); // efficient status_natural_heal processing

	iwall_db = strdb_alloc(DB_OPT_RELEASE_DATA,2*NAME_LENGTH+2+1); // [Zephyrus] Invisible Walls

#ifndef TXT_ONLY
	map_sql_init();
	if (log_config.sql_logs)
		log_sql_init();
#endif /* not TXT_ONLY */

	mapindex_init();
	if(enable_grf)
		grfio_init(GRF_PATH_FILENAME);

	map_readallmaps();

	add_timer_func_list(map_freeblock_timer, "map_freeblock_timer");
	add_timer_func_list(map_clearflooritem_timer, "map_clearflooritem_timer");
	add_timer_func_list(map_removemobs_timer, "map_removemobs_timer");
	add_timer_interval(gettick()+1000, map_freeblock_timer, 0, 0, 60*1000);

	do_init_atcommand();
	do_init_battle();
	do_init_instance();
	do_init_chrif();
	do_init_clif();
	do_init_script();
	do_init_itemdb();
	do_init_skill();
	do_init_mob();
	do_init_pc();
	do_init_status();
	do_init_party();
	do_init_guild();
	do_init_storage();
	do_init_pet();
	do_init_merc();
	do_init_mercenary();
	do_init_quest();
	do_init_npc();
	do_init_unit();
	do_init_battleground();

	npc_event_do_oninit();	// npcのOnInitイベント?行

	if( console )
	{
		//##TODO invoke a CONSOLE_START plugin event
	}

	if (battle_config.pk_mode == 1)
		ShowNotice("Server is running on '"CL_WHITE"PK Mode"CL_RESET"'.\n");

	ShowStatus("Server is '"CL_GREEN"ready"CL_RESET"' and listening on port '"CL_WHITE"%d"CL_RESET"'.\n\n", map_port);

	return 0;
}
