/**
 * @file   info.c
 * @author mathslinux <riegamaths@gmail.com>
 * @date   Sun May 27 19:48:58 2012
 *
 * @brief  Fetch QQ information. e.g. friends information, group information.
 *
 *
 */

#include <string.h>
#include <stdlib.h>
//to enable strptime
#define __USE_XOPEN
#include <time.h>
#include <utime.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "info.h"
#include "url.h"
#include "logger.h"
#include "http.h"
#include "smemory.h"
#include "json.h"
#include "async.h"
#include "util.h"

#define LWQQ_CACHE_DIR "/tmp/lwqq/"

static json_t *get_result_json_object(json_t *json);
static void create_post_data(LwqqClient *lc, char *buf, int buflen);
static int get_qqnumber_back(LwqqHttpRequest* req,LwqqGroup* group,LwqqBuddy* buddy);
static int get_avatar_back(LwqqHttpRequest* req,LwqqBuddy* buddy,LwqqGroup* group);
static int get_friends_info_back(LwqqHttpRequest* req);
static int get_online_buddies_back(LwqqHttpRequest* req,void* data);
static int get_group_name_list_back(LwqqHttpRequest* req,LwqqClient* lc);
static int group_detail_back(LwqqHttpRequest* req,LwqqClient* lc,LwqqGroup* group);
static int info_commom_back(LwqqHttpRequest* req,void* data);
static int get_discu_list_back(LwqqHttpRequest* req,void* data);
static int get_discu_detail_info_back(LwqqHttpRequest* req,LwqqClient* lc,LwqqGroup* discu);
static int get_friend_detail_back(LwqqHttpRequest* req,LwqqBuddy* buddy);
static void add_friend_stage_2(LwqqAsyncEvent* called,LwqqVerifyCode* code,LwqqBuddy* out);
static int process_result_default(LwqqHttpRequest* req);
static void add_group_stage_1(LwqqAsyncEvent* called,LwqqVerifyCode* code,LwqqGroup* g);
static int add_group_stage_2(LwqqHttpRequest* req,LwqqGroup* g);
static void add_group_stage_4(LwqqAsyncEvent* called,LwqqVerifyCode* c,LwqqGroup* g);


/**
 * Get the result object in a json object.
 *
 * @param str
 *
 * @return result object pointer on success, else NULL on failure.
 */
static json_t *get_result_json_object(json_t *json)
{
    json_t *json_tmp;
    char *value;

    /**
     * Frist, we parse retcode that indicate whether we get
     * correct response from server
     */
    value = json_parse_simple_value(json, "retcode");
    if (!value || strcmp(value, "0")) {
        goto failed ;
    }

    /**
     * Second, Check whether there is a "result" key in json object
     */
    json_tmp = json_find_first_label_all(json, "result");
    if (!json_tmp) {
        goto failed;
    }

    return json_tmp;

failed:
    return NULL;
}

/**
 * Just a utility function
 *
 * @param lc
 * @param buf
 * @param buflen
 */
static void create_post_data(LwqqClient *lc, char *buf, int buflen)
{
    char *s;
    char m[256];
    snprintf(m, sizeof(m), "{\"h\":\"hello\",\"vfwebqq\":\"%s\"}",
             lc->vfwebqq);
    s = url_encode(m);
    snprintf(buf, buflen, "r=%s", s);
    s_free(s);
}

/**
 * Parse friend category information
 *
 * @param lc
 * @param json Point to the first child of "result"'s value
 */
static void parse_categories_child(LwqqClient *lc, json_t *json)
{
    LwqqFriendCategory *cate;
    json_t *cur;

    /* Make json point category reference */
    while (json) {
        if (json->text && !strcmp(json->text, "categories")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        cate = s_malloc0(sizeof(*cate));
        cate->index = s_atoi(json_parse_simple_value(cur, "index"),0);
        cate->sort = s_atoi(json_parse_simple_value(cur, "sort"),0);
        cate->name = s_strdup(json_parse_simple_value(cur, "name"));

        /* Add to categories list */
        LIST_INSERT_HEAD(&lc->categories, cate, entries);
    }

    /* add the default category */
    cate = s_malloc0(sizeof(*cate));
    cate->index = 0;
    cate->name = s_strdup(LWQQ_DEFAULT_CATE);
    LIST_INSERT_HEAD(&lc->categories, cate, entries);
}

/**
 * Parse info child
 *
 * @param lc
 * @param json Point to the first child of "result"'s value
 */
static void parse_info_child(LwqqClient *lc, json_t *json)
{
    LwqqBuddy *buddy;
    json_t *cur;

    /* Make json point "info" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "info")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        buddy = lwqq_buddy_new();
        buddy->face = s_strdup(json_parse_simple_value(cur, "face"));
        buddy->flag = s_strdup(json_parse_simple_value(cur, "flag"));
        buddy->nick = json_unescape(json_parse_simple_value(cur, "nick"));
        buddy->uin = s_strdup(json_parse_simple_value(cur, "uin"));

        /* Add to buddies list */
        LIST_INSERT_HEAD(&lc->friends, buddy, entries);
    }
}

/**
 * Parse marknames child
 *
 * @param lc
 * @param json Point to the first child of "result"'s value
 */
static void parse_marknames_child(LwqqClient *lc, json_t *json)
{
    LwqqBuddy *buddy;
    char *uin, *markname;
    json_t *cur;

    /* Make json point "info" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "marknames")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        uin = json_parse_simple_value(cur, "uin");
        markname = json_parse_simple_value(cur, "markname");
        if (!uin || !markname)
            continue;

        buddy = lwqq_buddy_find_buddy_by_uin(lc, uin);
        if (!buddy)
            continue;

        /* Free old markname */
        if (buddy->markname)
            s_free(buddy->markname);
        buddy->markname = json_unescape(markname);
    }
}

/**
 * Parse friends child
 *
 * @param lc
 * @param json Point to the first child of "result"'s value
 */
static void parse_friends_child(LwqqClient *lc, json_t *json)
{
    LwqqBuddy *buddy;
    char *uin;
    json_t *cur;

    /* Make json point "info" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "friends")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

//    {"flag":0,"uin":1907104721,"categories":0}
    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        LwqqFriendCategory *c_entry;
        uin = json_parse_simple_value(cur, "uin");
        int cate_index = s_atoi(json_parse_simple_value(cur, "categories"),0);
        if (!uin || !cate_index)
            continue;

        buddy = lwqq_buddy_find_buddy_by_uin(lc, uin);
        if (!buddy)
            continue;

        LIST_FOREACH(c_entry, &lc->categories, entries) {
            if (c_entry->index == cate_index) {
                c_entry->count++;
            }
        }
        buddy->cate_index = cate_index;
    }
}

/**
 * Get QQ friends information. These information include basic friend
 * information, friends group information, and so on
 *
 * @param lc
 * @param err
 */
LwqqAsyncEvent* lwqq_info_get_friends_info(LwqqClient *lc, LwqqErrorCode *err)
{
    char msg[256] = {0};
    LwqqHttpRequest *req = NULL;

    /* Create post data: {"h":"hello","vfwebqq":"4354j53h45j34"} */
    create_post_data(lc, msg, sizeof(msg));

    /* Create a POST request */
    char url[512];
    snprintf(url, sizeof(url), "%s/api/get_user_friends2", "http://s.web2.qq.com");
    req = lwqq_http_create_default_request(lc,url, err);
    if (!req) {
        goto done;
    }
    req->set_header(req, "Referer", "http://s.web2.qq.com/proxy.html?v=20101025002");
    req->set_header(req, "Content-Transfer-Encoding", "binary");
    req->set_header(req, "Content-type", "application/x-www-form-urlencoded");
    req->set_header(req, "Cookie", lwqq_get_cookies(lc));
    return req->do_request_async(req, 1, msg,_C_(p_i,get_friends_info_back,req));

    /**
     * Here, we got a json object like this:
     * {"retcode":0,"result":{"friends":[{"flag":0,"uin":1907104721,"categories":0},{"flag":0,"uin":1745701153,"categories":0},{"flag":0,"uin":445284794,"categories":0},{"flag":0,"uin":4188952283,"categories":0},{"flag":0,"uin":276408653,"categories":0},{"flag":0,"uin":1526867107,"categories":0}],"marknames":[{"uin":276408653,"markname":""}],"categories":[{"index":1,"sort":1,"name":""},{"index":2,"sort":2,"name":""},{"index":3,"sort":3,"name":""}],"vipinfo":[{"vip_level":1,"u":1907104721,"is_vip":1},{"vip_level":1,"u":1745701153,"is_vip":1},{"vip_level":1,"u":445284794,"is_vip":1},{"vip_level":6,"u":4188952283,"is_vip":1},{"vip_level":0,"u":276408653,"is_vip":0},{"vip_level":1,"u":1526867107,"is_vip":1}],"info":[{"face":294,"flag":8389126,"nick":"","uin":1907104721},{"face":0,"flag":518,"nick":"","uin":1745701153},{"face":0,"flag":526,"nick":"","uin":445284794},{"face":717,"flag":8388614,"nick":"QQ","uin":4188952283},{"face":81,"flag":8389186,"nick":"Kernel","uin":276408653},{"face":0,"flag":2147484166,"nick":"Q","uin":1526867107}]}}
     *
     */
done:
    lwqq_http_request_free(req);
    return NULL;
}
static int get_friends_info_back(LwqqHttpRequest* req)
{
    json_t *json = NULL, *json_tmp;
    int ret = 0;
    int err = 0;
    LwqqClient* lc = req->lc;

    if (req->http_code != 200) {
        err = LWQQ_EC_HTTP_ERROR;
        goto done;
    }
    //force end with char zero.
    req->response[req->resp_len] = '\0';
    ret = json_parse_document(&json, req->response);
    if (ret != JSON_OK) {
        lwqq_log(LOG_ERROR, "Parse json object of friends error: %s\n", req->response);
        err = LWQQ_EC_ERROR;
        goto done;
    }

    json_tmp = get_result_json_object(json);
    if (!json_tmp) {
        lwqq_log(LOG_ERROR, "Parse json object error: %s\n", req->response);
        err = LWQQ_EC_ERROR;
        goto done;
    }
    /** It seems everything is ok, we start parsing information
     * now
     */
    if (json_tmp->child && json_tmp->child->child ) {
        json_tmp = json_tmp->child->child;

        /* Parse friend category information */
        parse_categories_child(lc, json_tmp);

        /**
         * Parse friends information.
         * Firse, we parse object's "info" child
         * Then, parse object's "marknames" child
         * Last, parse object's "friends" child
         */
        parse_info_child(lc, json_tmp);
        parse_marknames_child(lc, json_tmp);
        parse_friends_child(lc, json_tmp);
    }

done:
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return err;
}

LwqqAsyncEvent* lwqq_info_get_avatar(LwqqClient* lc,LwqqBuddy* buddy,LwqqGroup* group)
{
    static int serv_id = 0;
    if(!(lc&&(group||buddy))) return NULL;
    //there have avatar already do not repeat work;
    LwqqErrorCode error;
    int isgroup = group>0;
    const char* qqnumber = (isgroup)?group->account:buddy->qqnumber;
    const char* uin = (isgroup)?group->code:buddy->uin;

    //to avoid chinese character
    //setlocale(LC_TIME,"en_US.utf8");
    //first we try to read from disk
    char path[32];
    time_t modify=0;
    if(qqnumber || uin) {
        snprintf(path,sizeof(path),LWQQ_CACHE_DIR"%s",qqnumber?qqnumber:uin);
        struct stat st = {0};
        //we read it last modify date
        stat(path,&st);
        modify = st.st_mtime;
    }
    //we send request if possible with modify time
    //to reduce download rate
    LwqqHttpRequest* req;
    char url[512];
    char host[32];
    int type = (isgroup)?4:1;
    //there are face 1 to face 10 server to accelerate speed.
    snprintf(host,sizeof(host),"face%d.qun.qq.com",++serv_id);
    serv_id %= 10;
    snprintf(url, sizeof(url),
             "http://%s/cgi/svr/face/getface?cache=0&type=%d&fid=0&uin=%s&vfwebqq=%s",
             host,type,uin, lc->vfwebqq);
    req = lwqq_http_create_default_request(lc,url, &error);
    req->set_header(req, "Referer", "http://web2.qq.com/");
    req->set_header(req,"Host",host);

    //we ask server if it indeed need to update
    if(modify) {
        struct tm modify_tm;
        char buf[32];
        strftime(buf,sizeof(buf),"%a, %d %b %Y %H:%M:%S GMT",localtime_r(&modify,&modify_tm) );
        req->set_header(req,"If-Modified-Since",buf);
    }
    req->set_header(req, "Cookie", lwqq_get_cookies(lc));
    return req->do_request_async(req, 0, NULL,_C_(3p_i,get_avatar_back,req,buddy,group));
}
static int get_avatar_back(LwqqHttpRequest* req,LwqqBuddy* buddy,LwqqGroup* group)
{
    if( req == LWQQ_CALLBACK_FAILED ){
        return -1;
    }
    int isgroup = (group !=NULL);
    const char* qqnumber = (isgroup)?group->account:buddy->qqnumber;
    const char* uin = (isgroup)? group->code: buddy->uin;
    char** avatar = (isgroup)?&group->avatar:&buddy->avatar;
    size_t* len = (isgroup)?&group->avatar_len:&buddy->avatar_len;
    char path[32];
    int hasfile=0;
    size_t filesize=0;
    FILE* f;

    if(qqnumber || uin) {
        snprintf(path,sizeof(path),LWQQ_CACHE_DIR"%s",qqnumber?qqnumber:uin);
        struct stat st = {0};
        //we read it last modify date
        hasfile = !stat(path,&st);
        filesize = st.st_size;
    }

    if((req->http_code!=200 && req->http_code!=304)){
        goto done;
    }

    //ok we need update our cache because 
    //our cache outdate
    if(req->http_code != 304) {
        //we 'move' it instead of copy it
        *avatar = req->response;
        *len = req->resp_len;
        req->response = NULL;
        req->resp_len = 0;

        //we cache it to file
        if(qqnumber || uin ) {
            f = fopen(path,"w");
            if(f==NULL) {
                mkdir(LWQQ_CACHE_DIR,0777);
                f = fopen(path,"w");
            }

            fwrite(*avatar,1,*len,f);
            fclose(f);
            //we read last modify time from response header
            struct tm wtm = {0};
            strptime(req->get_header(req,"Last-Modified"),
                    "%a, %d %b %Y %H:%M:%S GMT",&wtm);
            //and write it to file
            struct utimbuf wutime;
            wutime.modtime = mktime(&wtm);
            wutime.actime = wutime.modtime;//it is not important
            utime(path,&wutime);
        }
        lwqq_http_request_free(req);
        return 0;
    }

done:
    //failed or we do not need update
    //we read from file
    if(hasfile){
        f = fopen(path,"r");
        if(f!=NULL){
            *avatar = s_malloc(filesize);
            fread(*avatar,1,filesize,f);
            *len = filesize;
        }else{
            perror("打开头像文件失败");
        }
    }
    lwqq_http_request_free(req);
    return 0;
}

/**
 * Parsing group info like this.
 *
 * "gnamelist":[
 *  {"flag":17825793,"name":"EGE...C/C++............","gid":3772225519,"code":1713443374},
 *  {"flag":1,"name":"............","gid":2698833507,"code":3968641865}
 * ]
 *
 * @param lc
 * @param json Point to the first child of "result"'s value
 */
static void parse_groups_gnamelist_child(LwqqClient *lc, json_t *json)
{
    LwqqGroup *group;
    json_t *cur;

    /* Make json point "gnamelist" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "gnamelist")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        group = lwqq_group_new(0);
        group->flag = s_strdup(json_parse_simple_value(cur, "flag"));
        group->name = json_unescape(json_parse_simple_value(cur, "name"));
        group->gid = s_strdup(json_parse_simple_value(cur, "gid"));
        group->code = s_strdup(json_parse_simple_value(cur, "code"));

        /* we got the 'code', so we can get the qq group number now */
        //group->account = get_group_qqnumber(lc, group->code);

        /* Add to groups list */
        LIST_INSERT_HEAD(&lc->groups, group, entries);
    }
}

static void parse_groups_gmasklist_child(LwqqClient *lc, json_t *json)
{
    while (json) {
        if (json->text && !strcmp(json->text, "gmasklist")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }
    json = json->child->child;

    const char* gid;
    int mask;
    LwqqGroup* group;
    while(json){
        gid = json_parse_simple_value(json,"gid");
        mask = s_atoi(json_parse_simple_value(json,"mask"),LWQQ_MASK_NONE);

        group = lwqq_group_find_group_by_gid(lc,gid);
        if(group){
            group->mask = mask;
        }
        json = json->next;
    }
}
/**
 * Parse group info like this
 *
 * "gmarklist":[{"uin":2698833507,"markname":".................."}]
 *
 * @param lc
 * @param json Point to the first child of "result"'s value
 */
static void parse_groups_gmarklist_child(LwqqClient *lc, json_t *json)
{
    LwqqGroup *group;
    json_t *cur;
    char *uin;
    char *markname;

    /* Make json point "gmarklist" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "gmarklist")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        uin = json_parse_simple_value(cur, "uin");
        markname = json_parse_simple_value(cur, "markname");

        if (!uin || !markname)
            continue;
        group = lwqq_group_find_group_by_gid(lc, uin);
        if (!group)
            continue;
        group->markname = json_unescape(markname);
    }
}

/**
 * Get QQ groups' name information. Get only 'name', 'gid' , 'code' .
 *
 * @param lc
 * @param err
 */
LwqqAsyncEvent* lwqq_info_get_group_name_list(LwqqClient *lc, LwqqErrorCode *err)
{

    lwqq_log(LOG_DEBUG, "in function.");

    char msg[256];
    char url[512];
    LwqqHttpRequest *req = NULL;

    /* Create post data: {"h":"hello","vfwebqq":"4354j53h45j34"} */
    create_post_data(lc, msg, sizeof(msg));

    /* Create a POST request */
    snprintf(url, sizeof(url), "%s/api/get_group_name_list_mask2", "http://s.web2.qq.com");
    req = lwqq_http_create_default_request(lc,url, err);
    if (!req) {
        goto done;
    }
    req->set_header(req, "Referer", "http://s.web2.qq.com/proxy.html?v=20101025002");
    req->set_header(req, "Content-Transfer-Encoding", "binary");
    req->set_header(req, "Content-type", "application/x-www-form-urlencoded");
    req->set_header(req, "Cookie", lwqq_get_cookies(lc));
    return req->do_request_async(req, 1, msg,_C_(2p_i,get_group_name_list_back,req,lc));
done:
    lwqq_http_request_free(req);
    return NULL;
}
static int get_group_name_list_back(LwqqHttpRequest* req,LwqqClient* lc)
{
    json_t *json = NULL, *json_tmp;
    int ret=0;
    int err=0;

    if (req->http_code != 200) {
        err = LWQQ_EC_HTTP_ERROR;
        goto done;
    }

    /**
     * Here, we got a json object like this:
     * {"retcode":0,"result":{
     * "gmasklist":[],
     * "gnamelist":[
     *  {"flag":17825793,"name":"EGE...C/C++............","gid":3772225519,"code":1713443374},
     *  {"flag":1,"name":"............","gid":2698833507,"code":3968641865}
     * ],
     * "gmarklist":[{"uin":2698833507,"markname":".................."}]}}
     *
     */
    req->response[req->resp_len] = '\0';
    ret = json_parse_document(&json, req->response);
    if (ret != JSON_OK) {
        lwqq_log(LOG_ERROR, "Parse json object of groups error: %s\n", req->response);
        err = LWQQ_EC_ERROR;
        goto done;
    }

    json_tmp = get_result_json_object(json);
    if (!json_tmp) {
        lwqq_log(LOG_ERROR, "Parse json object error: %s\n", req->response);
        err = LWQQ_EC_ERROR;
        goto done;
    }

    /** It seems everything is ok, we start parsing information
     * now
     */
    if (json_tmp->child && json_tmp->child->child ) {
        json_tmp = json_tmp->child->child;

        /* Parse friend category information */
        parse_groups_gnamelist_child(lc, json_tmp);
        parse_groups_gmasklist_child(lc, json_tmp);
        parse_groups_gmarklist_child(lc, json_tmp);

    }

done:
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return err;

}

LwqqAsyncEvent* lwqq_info_get_discu_name_list(LwqqClient* lc)
{
    if(!lc) return NULL;

    char url[512];
    snprintf(url,sizeof(url),"http://d.web2.qq.com/channel/get_discu_list_new2?clientid=%s&psessionid=%s&vfwebqq=%s&t=%ld",
            lc->clientid,lc->psessionid,lc->vfwebqq,time(NULL));
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url, NULL);
    req->set_header(req,"Referer","http://d.web2.qq.com/proxy.html?v=20110331002&id=2");
    req->set_header(req,"Cookie",lwqq_get_cookies(lc));
    
    return req->do_request_async(req,0,NULL,_C_(2p_i,get_discu_list_back,req,lc));
}

static void parse_discus_discu_child(LwqqClient* lc,json_t* root)
{
    json_t* json = json_find_first_label(root, "dnamelist");
    if(json == NULL) return;
    json = json->child->child;
    char* name;
    while(json){
        LwqqGroup* discu = lwqq_group_new(LWQQ_GROUP_DISCU);
        discu->did = s_strdup(json_parse_simple_value(json,"did"));
        //for compability
        discu->account = s_strdup(discu->did);
        name = json_parse_simple_value(json,"name");
        if(strcmp(name,"")==0)
            discu->name = s_strdup("未命名讨论组");
        else
            discu->name = json_unescape(name);
        LIST_INSERT_HEAD(&lc->discus, discu, entries);
        json=json->next;
    }

    json = json_find_first_label(root,"dmasklist");
    if(json == NULL) return;
    json = json->child->child;
    const char* did;
    int mask;
    LwqqGroup* group;
    while(json){
        did = json_parse_simple_value(json,"did");
        mask = s_atoi(json_parse_simple_value(json,"mask"),LWQQ_MASK_NONE);
        group = lwqq_group_find_group_by_gid(lc,did);
        if(group!=NULL) group->mask = mask;
        json = json->next;
    }
}

static int get_discu_list_back(LwqqHttpRequest* req,void* data)
{
    LwqqClient* lc = data;
    int errno=0;
    json_t* root = NULL;
    json_t* json_temp = NULL;
    if(req->http_code!=200){
        errno = 1;
        goto done;
    }
    req->response[req->resp_len] = '\0';
    json_parse_document(&root, req->response);
    if(!root){ errno = 1; goto done;}
    json_temp = get_result_json_object(root);
    if(!json_temp) {errno=1;goto done;}

    if (json_temp->child && json_temp->child->child ) {

        json_temp = json_temp->child;
        parse_discus_discu_child(lc,json_temp);
    }


done:
    if(root)
        json_free_value(&root);
    lwqq_http_request_free(req);
    return errno;
}

/**
 * Get all friends qqnumbers
 *
 * @param lc
 * @param err
 */
void lwqq_info_get_all_friend_qqnumbers(LwqqClient *lc, LwqqErrorCode *err)
{
    LwqqBuddy *buddy;

    if (!lc)
        return ;

    LIST_FOREACH(buddy, &lc->friends, entries) {
        if (!buddy->qqnumber) {
            /** If qqnumber hasnt been fetched(NB: lc->myself has qqnumber),
             * fetch it
             */
            lwqq_info_get_friend_qqnumber(lc,buddy);
            //lwqq_log(LOG_DEBUG, "Get buddy qqnumber: %s\n", buddy->qqnumber);
        }
    }

    if (err) {
        *err = LWQQ_EC_OK;
    }
}


/**
 * Parse group info like this
 * Is the "members" in "ginfo" useful ? Here not parsing it...
 *
 * "result":{
 * "ginfo":
 *   {"face":0,"memo":"","class":10026,"fingermemo":"","code":3968641865,"createtime":1339647698,"flag":1,
 *    "level":0,"name":"............","gid":2698833507,"owner":909998471,
 *    "members":[{"muin":56360327,"mflag":0},{"muin":909998471,"mflag":0}],
 *    "option":2},
 *
 * @param lc
 * @param group
 * @param json Point to the first child of "result"'s value
 */
static void parse_groups_ginfo_child(LwqqClient *lc, LwqqGroup *group,  json_t *json)
{
    char *gid;

    /* Make json point "ginfo" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "ginfo")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    //json = json->child;    // there's no array here , comment it.
    gid = json_parse_simple_value(json,"gid");
    if (strcmp(group->gid, gid) != 0) {
        lwqq_log(LOG_ERROR, "Parse json object error.");
        return;
    }

#define  SET_GINFO(key, name) {                                    \
        if (group->key) {                                               \
            s_free(group->key);                                         \
        }                                                               \
        group->key = s_strdup(json_parse_simple_value(json, name));     \
    }

    /* we have got the 'code','name' and 'gid', so we comment it here. */
    SET_GINFO(face, "face");
    SET_GINFO(memo, "memo");
    SET_GINFO(class, "class");
    SET_GINFO(fingermemo,"fingermemo");
    //SET_GINFO(code, "code");
    SET_GINFO(createtime, "createtime");
    SET_GINFO(flag, "flag");
    SET_GINFO(level, "level");
    //SET_GINFO(name, "name");
    //SET_GINFO(gid, "gid");
    SET_GINFO(owner, "owner");
    SET_GINFO(option, "option");

#undef SET_GINFO

}
static char* ibmpc_ascii_character_convert(char *str)
{
    static char buf[256];
    char *ptr = str;
    char *write = buf;
    const char* spec;
    while(*ptr!='\0'){
        switch(*ptr){
            case 0x01:spec="☺";break;
            case 0x02:spec="☻";break;
            case 0x03:spec="♥";break;
            case 0x04:spec="◆";break;
            case 0x05:spec="♣";break;
            case 0x06:spec="♠";break;
            case 0x07:spec="⚫";break;
            case 0x08:spec="⚫";break;
            case 0x09:spec="⚪";break;
            case 0x0A:spec="⚪";break;
            case 0x0B:spec="♂";break;
            case 0x0C:spec="♁";break;
            case 0x0D:spec="♪";break;
            case 0x0E:spec="♬";break;
            case 0x0F:spec="☼";break;
            case 0x10:spec="▶";break;
            case 0x11:spec="◀";break; 
            case 0x12:spec="↕";break;
            case 0x13:spec="‼";break;
            case 0x14:spec="¶";break;
            case 0x15:spec="§";break;
            case 0x16:spec="▁";break;
            case 0x17:spec="↨";break;
            case 0x18:spec="↑";break;
            case 0x19:spec="↓";break;
            case 0x1A:spec="→";break;
            case 0x1B:spec="←";break;
            case 0x1C:spec="∟";break;
            case 0x1D:spec="↔";break;
            case 0x1E:spec="▲";break;
            case 0x1F:spec="▼";break;
            default:
                *write++ = *ptr;
                ptr++;
                continue;
                break;
        }
        strcpy(write,spec);
        write+=strlen(write);
        ptr++;
    }
    *write = '\0';
    s_free(str);
    return s_strdup(buf);
}
/**
 * Parse group members info
 * we only get the "nick" and the "uin", and get the members' qq number.
 *
 * "minfo":[
 *   {"nick":"evildoer","province":"......","gender":"male","uin":56360327,"country":"......","city":"......"},
 *   {"nick":"evil...doer","province":"......","gender":"male","uin":909998471,"country":"......","city":"......"}],
 *
 * @param lc
 * @param group
 * @param json Point to the first child of "result"'s value
 */
static void parse_groups_minfo_child(LwqqClient *lc, LwqqGroup *group,  json_t *json)
{
    LwqqSimpleBuddy *member;
    json_t *cur;
    char *uin;
    char *nick;

    /* Make json point "minfo" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "minfo")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        uin = json_parse_simple_value(cur, "uin");
        nick = json_parse_simple_value(cur, "nick");

        if (!uin || !nick)
            continue;

        //may solve group member duplicated problem
        if(lwqq_group_find_group_member_by_uin(group,uin)==NULL){

            member = lwqq_simple_buddy_new();

            member->uin = s_strdup(uin);
            member->nick = ibmpc_ascii_character_convert(json_unescape(nick));

            /* Add to members list */
            LIST_INSERT_HEAD(&group->members, member, entries);
        }
    }
}
static void parse_groups_ginfo_members_child(LwqqClient *lc, LwqqGroup *group,  json_t *json)
{
    while (json) {
        if (json->text && !strcmp(json->text, "ginfo")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }
    json_t* members = json_find_first_label_all(json,"members");
    members = members->child->child;
    const char* uin;
    int mflag;
    LwqqSimpleBuddy* sb;
    while(members){
        uin = json_parse_simple_value(members,"muin");
        mflag = s_atoi(json_parse_simple_value(members,"mflag"),0);
        sb = lwqq_group_find_group_member_by_uin(group,uin);
        if(sb) sb->mflag = mflag;

        members = members->next;
    }

}
static void parse_groups_cards_child(LwqqClient *lc, LwqqGroup *group,  json_t *json)
{
    LwqqSimpleBuddy *member;
    json_t *cur;
    char *uin;
    char *card;

    /* Make json point "minfo" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "cards")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        uin = json_parse_simple_value(cur, "muin");
        card = json_parse_simple_value(cur, "card");

        if (!uin || !card)
            continue;

        member = lwqq_group_find_group_member_by_uin(group,uin);
        if(member != NULL){
            member->card = ibmpc_ascii_character_convert(json_unescape(card));
        }
    }
}

/**
 * mark qq group's online members
 *
 * "stats":[{"client_type":1,"uin":56360327,"stat":10},{"client_type":41,"uin":909998471,"stat":10}],
 *
 * @param lc
 * @param group
 * @param json Point to the first child of "result"'s value
 */
static void parse_groups_stats_child(LwqqClient *lc, LwqqGroup *group,  json_t *json)
{
    LwqqSimpleBuddy *member;
    json_t *cur;
    char *uin;

    /* Make json point "stats" reference */
    while (json) {
        if (json->text && !strcmp(json->text, "stats")) {
            break;
        }
        json = json->next;
    }
    if (!json) {
        return ;
    }

    json = json->child;    //point to the array.[]
    for (cur = json->child; cur != NULL; cur = cur->next) {
        uin = json_parse_simple_value(cur, "uin");

        if (!uin)
            continue;
        member = lwqq_group_find_group_member_by_uin(group, uin);
        if (!member)
            continue;
        member->client_type = s_atoi(json_parse_simple_value(cur, "client_type"),LWQQ_CLIENT_DESKTOP);
        member->stat = s_atoi(json_parse_simple_value(cur, "stat"),LWQQ_STATUS_LOGOUT);

    }
}

/**
 * Get QQ groups detail information.
 *
 * @param lc
 * @param group
 * @param err
 */
LwqqAsyncEvent* lwqq_info_get_group_detail_info(LwqqClient *lc, LwqqGroup *group,
                                     LwqqErrorCode *err)
{
    lwqq_log(LOG_DEBUG, "in function.");

    char url[512];
    LwqqHttpRequest *req = NULL;

    if (!lc || ! group) {
        return NULL;
    }

    LwqqAsyncEvent* ev = lwqq_async_queue_find(&group->ev_queue,lwqq_info_get_group_detail_info);
    if(ev) return ev;

    if(group->type == LWQQ_GROUP_QUN){
        /* Make sure we know code. */
        if (!group->code) {
            if (err)
                *err = LWQQ_EC_NULL_POINTER;
            return NULL;
        }

        /* Create a GET request */
        snprintf(url, sizeof(url),
                "%s/api/get_group_info_ext2?gcode=%s&vfwebqq=%s&t=%ld",
                "http://s.web2.qq.com", group->code, lc->vfwebqq,time(NULL));
        req = lwqq_http_create_default_request(lc,url, err);
        req->set_header(req, "Referer", "http://s.web2.qq.com/proxy.html?v=20110412001&id=3");

    }else if(group->type == LWQQ_GROUP_DISCU){

        snprintf(url,sizeof(url),
                "http://d.web2.qq.com/channel/get_discu_info?"
                "did=%s&clientid=%s&psessionid=%s&t=%ld",
                group->did,lc->clientid,lc->psessionid,time(NULL));
        req = lwqq_http_create_default_request(lc,url,NULL);
        req->set_header(req,"Referer","http://d.web2.qq.com/proxy.html?v=20110331002&id=2");
    }
    req->set_header(req, "Cookie", lwqq_get_cookies(lc));
    ev = req->do_request_async(req, 0, NULL,_C_(3p_i,group_detail_back,req,lc,group));
    lwqq_async_queue_add(&group->ev_queue,lwqq_info_get_group_detail_info,ev);
    return ev;
}

static int group_detail_back(LwqqHttpRequest* req,LwqqClient* lc,LwqqGroup* group)
{
    lwqq_async_queue_rm(&group->ev_queue,lwqq_info_get_group_detail_info);
    if(group->type == LWQQ_GROUP_DISCU){
        return get_discu_detail_info_back(req,lc,group);
    }
    int ret;
    int errno = 0;
    json_t *json = NULL, *json_tmp;
    if (req->http_code != 200) {
        errno = LWQQ_EC_HTTP_ERROR;
        goto done;
    }

    /**
     * Here, we got a json object like this:
     *
     * {"retcode":0,
     * "result":{
     * "stats":[{"client_type":1,"uin":56360327,"stat":10},{"client_type":41,"uin":909998471,"stat":10}],
     * "minfo":[
     *   {"nick":"evildoer","province":"......","gender":"male","uin":56360327,"country":"......","city":"......"},
     *   {"nick":"evil...doer","province":"......","gender":"male","uin":909998471,"country":"......","city":"......"}
     *   {"nick":"glrapl","province":"","gender":"female","uin":1259717843,"country":"","city":""}],
     * "ginfo":
     *   {"face":0,"memo":"","class":10026,"fingermemo":"","code":3968641865,"createtime":1339647698,"flag":1,
     *    "level":0,"name":"............","gid":2698833507,"owner":909998471,
     *    "members":[{"muin":56360327,"mflag":0},{"muin":909998471,"mflag":0}],
     *    "option":2},
     * "cards":[{"muin":3777107595,"card":""},{"muin":3437728033,"card":":FooTearth"}],
     * "vipinfo":[{"vip_level":0,"u":56360327,"is_vip":0},{"vip_level":0,"u":909998471,"is_vip":0}]}}
     *
     */
    req->response[req->resp_len] = '\0';
    ret = json_parse_document(&json, req->response);
    if (ret != JSON_OK) {
        lwqq_log(LOG_ERROR, "Parse json object of groups error: %s\n", req->response);
        errno = LWQQ_EC_ERROR;
        goto done;
    }

    json_tmp = get_result_json_object(json);
    if (!json_tmp) {
        lwqq_log(LOG_ERROR, "Parse json object error: %s\n", req->response);
        goto json_error;
    }

    /** It seems everything is ok, we start parsing information
     * now
     */
    if (json_tmp->child && json_tmp->child->child ) {
        json_tmp = json_tmp->child->child;

        /* first , get group information */
        parse_groups_ginfo_child(lc, group, json_tmp);
        /* second , get group members */
        parse_groups_minfo_child(lc, group, json_tmp);
        parse_groups_ginfo_members_child(lc,group,json_tmp);
        parse_groups_cards_child(lc, group, json_tmp);
        /* third , mark group's online members */
        parse_groups_stats_child(lc, group, json_tmp);

    }

done:
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return errno;

json_error:
    errno = LWQQ_EC_ERROR;
    /* Free temporary string */
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return errno;
}

static void parse_discus_info_child(LwqqClient* lc,LwqqGroup* discu,json_t* root)
{
    json_t* json = json_find_first_label(root,"info");
    json = json->child;

    discu->owner = s_strdup(json_parse_simple_value(json,"discu_owner"));

    json = json_find_first_label(json,"mem_list");
    json = json->child->child;
    while(json){
        LwqqSimpleBuddy* sb = lwqq_simple_buddy_new();
        sb->uin = s_strdup(json_parse_simple_value(json,"mem_uin"));
        sb->qq = s_strdup(json_parse_simple_value(json,"ruin"));
        LIST_INSERT_HEAD(&discu->members,sb,entries);
        json = json->next;
    }
}
static void parse_discus_other_child(LwqqClient* lc,LwqqGroup* discu,json_t* root)
{
    json_t* json = json_find_first_label(root,"mem_info");

    json = json->child->child;
    const char* uin;

    while(json){
        uin = json_parse_simple_value(json,"uin");
        LwqqSimpleBuddy* sb = lwqq_group_find_group_member_by_uin(discu,uin);
        sb->nick = json_unescape(json_parse_simple_value(json,"nick"));
        json = json->next;
    }

    json = json_find_first_label(root,"mem_status");
    
    json = json->child->child;

    while(json){
        uin = json_parse_simple_value(json,"uin");
        LwqqSimpleBuddy* sb = lwqq_group_find_group_member_by_uin(discu,uin);
        sb->client_type = s_atoi(json_parse_simple_value(json,"client_type"),LWQQ_CLIENT_DESKTOP);
        sb->stat = lwqq_status_from_str(json_parse_simple_value(json,"status"));
        json = json->next;
    }
}
static int get_discu_detail_info_back(LwqqHttpRequest* req,LwqqClient* lc,LwqqGroup* discu)
{
    int errno = 0;
    json_t* root = NULL,* json = NULL;

    if(req->http_code!=200){
        errno = 1;
        goto done;
    }
    
    req->response[req->resp_len] = '\0';
    json_parse_document(&root,req->response);
    json = get_result_json_object(root);
    if(json && json->child) {
        json = json->child;

        parse_discus_info_child(lc,discu,json);
        parse_discus_other_child(lc,discu,json);
    }
done:
    if(root)
        json_free_value(&root);
    lwqq_http_request_free(req);
    return errno;
}

LwqqAsyncEvent* lwqq_info_get_qqnumber(LwqqClient* lc,int isgroup,void* grouporbuddy)
{
    if (!lc || !grouporbuddy) {
        return NULL;
    }
    LwqqBuddy* buddy = NULL;
    LwqqGroup* group = NULL;
    const char* uin = NULL;
    if(isgroup){
        group = grouporbuddy;
        uin = group->code;
    }else{
        buddy = grouporbuddy;
        uin = buddy->uin;
    }


    char url[512];
    LwqqHttpRequest *req = NULL;
    snprintf(url, sizeof(url),
             "%s/api/get_friend_uin2?tuin=%s&verifysession=&type=1&code=&vfwebqq=%s&t=%ld",
             "http://s.web2.qq.com", uin, lc->vfwebqq,time(NULL));
    req = lwqq_http_create_default_request(lc,url, NULL);
    req->set_header(req, "Referer", "http://s.web2.qq.com/proxy.html?v=20110412001&id=3");
    req->set_header(req, "Cookie", lwqq_get_cookies(lc));
    return req->do_request_async(req, 0, NULL,_C_(3p_i,get_qqnumber_back,req,group,buddy));
}
static int get_qqnumber_back(LwqqHttpRequest* req,LwqqGroup* group,LwqqBuddy* buddy)
{
    int isgroup = (group!=NULL);

    char* account;
    int ret;
    json_t* json=NULL;
    int errno = 0;
    const char* retcode;
    if (req->http_code != 200) {
        lwqq_log(LOG_ERROR, "qqnumber response error: %s\n", req->response);
        errno = 1;
        goto done;
    }

    /**
     * Here, we got a json object like this:
     * {"retcode":0,"result":{"uiuin":"","account":615050000,"uin":954663841}}
     *
     */
    ret = json_parse_document(&json, req->response);
    if (ret != JSON_OK) {
        lwqq_log(LOG_ERROR, "Parse json object of groups error: %s\n", req->response);
        errno = 1;
        goto done;
    }
    retcode = json_parse_simple_value(json,"retcode");
    errno = s_atoi(retcode,1);
    if(errno!=0){
        lwqq_log(LOG_ERROR," qqnumber fetch error: retcode %s\n",retcode);
        goto done;
    }
    //uin = json_parse_simple_value(json,"uin");
    account = json_parse_simple_value(json,"account");

    if(isgroup){
        group->account = s_strdup(account);
    }else{ 
        buddy->qqnumber = s_strdup(account);
    }
done:
    /* Free temporary string */
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return errno;
}


/**
 * Get detail information of QQ friend(NB: include myself)
 * QQ server need us to pass param like:
 * tuin=244569070&verifysession=&code=&vfwebqq=e64da25c140c66
 *
 * @param lc
 * @param buddy
 * @param err
 */
LwqqAsyncEvent* lwqq_info_get_friend_detail_info(LwqqClient *lc, LwqqBuddy *buddy)
{
    lwqq_log(LOG_DEBUG, "in function.");

    char url[512];
    LwqqHttpRequest *req = NULL;

    if (!lc || ! buddy) {
        return NULL;
    }

    /* Make sure we know uin. */
    if (!buddy->uin) {
        return NULL;
    }

    /* Create a GET request */
    snprintf(url, sizeof(url),
             "%s/api/get_friend_info2?tuin=%s&verifysession=&code=&vfwebqq=%s",
             "http://s.web2.qq.com", buddy->uin, lc->vfwebqq);
    req = lwqq_http_create_default_request(lc,url, NULL);
    req->set_header(req, "Referer", "http://s.web2.qq.com/proxy.html?v=20101025002");
    req->set_header(req, "Content-Transfer-Encoding", "binary");
    req->set_header(req, "Content-type", "utf-8");
    req->set_header(req, "Cookie", lwqq_get_cookies(lc));
    return req->do_request_async(req, 0, NULL,_C_(2p_i,get_friend_detail_back,req,buddy));
}
static void parse_friend_detail_by_json(LwqqBuddy* buddy,json_t* json)
{
    //{"retcode":0,"result":{"face":567,"birthday":{"month":6,"year":1991,"day":14},"occupation":"","phone":"","allow":1,"college":"","uin":289056851,"constel":5,"blood":0,"homepage":"","stat":10,"vip_info":0,"country":"中国","city":"威海","personal":"","nick":"d3dd","shengxiao":8,"email":"","client_type":41,"province":"山东","gender":"male","mobile":""}}
    //
#define  SET_BUDDY_INFO(key, name) {                                    \
            s_free(buddy->key);                                         \
            buddy->key = s_strdup(json_parse_simple_value(json, name)); \
        }
        SET_BUDDY_INFO(uin, "uin");
        SET_BUDDY_INFO(face, "face");
        json_t* birth_tmp = json_find_first_label(json, "birthday");
        if(birth_tmp && birth_tmp->child ){
            birth_tmp = birth_tmp->child;
            struct tm tm_ = {0};
            tm_.tm_year = s_atoi(json_parse_simple_value(birth_tmp, "year"),1991)-1900;
            tm_.tm_mon = s_atoi(json_parse_simple_value(birth_tmp, "month"), 1) -1;
            tm_.tm_mday = s_atoi(json_parse_simple_value(birth_tmp, "day"), 1);
            time_t t = mktime(&tm_);
            buddy->birthday = t;
        }
        SET_BUDDY_INFO(occupation, "occupation");
        SET_BUDDY_INFO(phone, "phone");
        SET_BUDDY_INFO(allow, "allow");
        SET_BUDDY_INFO(college, "college");
        SET_BUDDY_INFO(reg_time, "reg_time");
        SET_BUDDY_INFO(constel, "constel");
        SET_BUDDY_INFO(blood, "blood");
        SET_BUDDY_INFO(homepage, "homepage");
        buddy->stat = s_atoi(json_parse_simple_value(json,"stat"),LWQQ_STATUS_LOGOUT);
        SET_BUDDY_INFO(vip_info, "vip_info");
        SET_BUDDY_INFO(country, "country");
        SET_BUDDY_INFO(city, "city");
        SET_BUDDY_INFO(personal, "personal");
        SET_BUDDY_INFO(nick, "nick");
        SET_BUDDY_INFO(shengxiao, "shengxiao");
        SET_BUDDY_INFO(email, "email");
        buddy->client_type = s_atoi(json_parse_simple_value(json,"client_type"),LWQQ_CLIENT_DESKTOP);
        SET_BUDDY_INFO(province, "province");
        SET_BUDDY_INFO(gender, "gender");
        SET_BUDDY_INFO(mobile, "mobile");
        SET_BUDDY_INFO(token, "token");
#undef SET_BUDDY_INFO
}
/*
static int get_friend_detail_back(LwqqHttpRequest* req,LwqqBuddy* buddy)
{
    json_t *json = NULL, *json_tmp;
    int err = 0;
    int ret;
    if (req->http_code != 200) {
        err = LWQQ_EC_HTTP_ERROR;
        goto done;
    }

    // **
     * Here, we got a json object like this:
     * {"retcode":0,"result":{"face":519,"birthday":
     * {"month":9,"year":1988,"day":26},"occupation":"学生",
     * "phone":"82888909","allow":1,"college":"西北工业大学","reg_time":0,
     * "uin":1421032531,"constel":8,"blood":2,
     * "homepage":"http://www.ifeng.com","stat":10,"vip_info":0,
     * "country":"中国","city":"西安","personal":"给力啊~~","nick":"阿凡达",
     * "shengxiao":5,"email":"avata@126.com","client_type":41,
     * "province":"陕西","gender":"male","mobile":"139********"}}
     *
     * /
    ret = json_parse_document(&json, req->response);
    if (ret != JSON_OK) {
        lwqq_log(LOG_ERROR, "Parse json object of groups error: %s\n", req->response);
        err = LWQQ_EC_ERROR;
        goto done;
    }

    json_tmp = get_result_json_object(json);
    if (!json_tmp) {
        lwqq_log(LOG_ERROR, "Parse json object error: %s\n", req->response);
        err = LWQQ_EC_ERROR;
        goto done;
    }

    / ** It seems everything is ok, we start parsing information
     * now
     * /
    if (json_tmp->child) {
        json_tmp = json_tmp->child;
        parse_friend_detail_by_json(buddy, json_tmp);
    }

done:
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return err;
}*/

static void update_online_buddies(LwqqClient *lc, json_t *json)
{
    /**
     * The json object is:
     * [{"uin":1100872453,"status":"online","client_type":21},"
     * "{"uin":2726159277,"status":"busy","client_type":1}]
     */
    json_t *cur;
    for (cur = json->child; cur != NULL; cur = cur->next) {
        char *uin, *status, *client_type;
        LwqqBuddy *b;
        uin = json_parse_simple_value(cur, "uin");
        status = json_parse_simple_value(cur, "status");
        if (!uin || !status) {
            continue;
        }
        client_type = json_parse_simple_value(cur, "client_type");
        b = lwqq_buddy_find_buddy_by_uin(lc, uin);
        if (b) {
            b->stat = lwqq_status_from_str(status);
            if (client_type) {
                b->client_type = s_atoi(client_type,LWQQ_CLIENT_DESKTOP);
            }
        }
    }
}

/**
 * Get online buddies
 * NB : This function must be called after lwqq_info_get_friends_info()
 * because we stored buddy's status in buddy object which is created in
 * lwqq_info_get_friends_info()
 *
 * @param lc
 * @param err
 */
LwqqAsyncEvent* lwqq_info_get_online_buddies(LwqqClient *lc, LwqqErrorCode *err)
{
    char url[512];
    LwqqHttpRequest *req = NULL;

    if (!lc) {
        return NULL;
    }

    /* Create a GET request */
    snprintf(url, sizeof(url),
             "%s/channel/get_online_buddies2?clientid=%s&psessionid=%s",
             "http://d.web2.qq.com", lc->clientid, lc->psessionid);
    req = lwqq_http_create_default_request(lc,url, err);
    if (!req) {
        goto done;
    }
    req->set_header(req, "Referer", "http://d.web2.qq.com/proxy.html?v=20101025002");
    req->set_header(req, "Content-Transfer-Encoding", "binary");
    req->set_header(req, "Content-type", "utf-8");
    req->set_header(req, "Cookie", lwqq_get_cookies(lc));
    return req->do_request_async(req, 0, NULL,_C_(2p_i,get_online_buddies_back,req,lc));
done:
    lwqq_http_request_free(req);
    return NULL;
}
static int get_online_buddies_back(LwqqHttpRequest* req,void* data)
{
    json_t *json = NULL, *json_tmp;
    int ret;
    LwqqClient* lc = data;
    LwqqErrorCode error;
    LwqqErrorCode* err = &error;

    if (req->http_code != 200) {
        if (err)
            *err = LWQQ_EC_HTTP_ERROR;
        goto done;
    }

    /**
     * Here, we got a json object like this:
     * {"retcode":0,"result":[{"uin":1100872453,"status":"online","client_type":21},"
     * "{"uin":2726159277,"status":"busy","client_type":1}]}
    */
    ret = json_parse_document(&json, req->response);
    if (ret != JSON_OK) {
        lwqq_log(LOG_ERROR, "Parse json object of groups error: %s\n", req->response);
        if (err)
            *err = LWQQ_EC_ERROR;
        goto done;
    }

    json_tmp = get_result_json_object(json);
    if (!json_tmp) {
        lwqq_log(LOG_ERROR, "Parse json object error: %s\n", req->response);
        goto json_error;
    }

    if (json_tmp->child) {
        json_tmp = json_tmp->child;
        update_online_buddies(lc, json_tmp);
    }

    lwqq_puts("online buddies complete");
done:
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return 0;

json_error:
    if (err)
        *err = LWQQ_EC_ERROR;
    /* Free temporary string */
    if (json)
        json_free_value(&json);
    lwqq_http_request_free(req);
    return 0;
}
enum CHANGE{
    CHANGE_BUDDY_MARKNAME,
    CHANGE_GROUP_MARKNAME,
    MODIFY_BUDDY_CATEGORY,
    CHANGE_STATUS,
    CHANGE_MASK
};
LwqqAsyncEvent* lwqq_info_change_buddy_markname(LwqqClient* lc,LwqqBuddy* buddy,const char* alias)
{
    if(!lc||!buddy||!alias) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/change_mark_name2","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"tuin=%s&markname=%s&vfwebqq=%s",
            buddy->uin,alias,lc->vfwebqq
            );
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    void** data = s_malloc0(sizeof(void*)*3);
    data[0] = (void*)CHANGE_BUDDY_MARKNAME;
    data[1] = buddy;
    data[2] = s_strdup(alias);
    return req->do_request_async(req,1,post,_C_(2p_i,info_commom_back,req,data));
done:
    lwqq_http_request_free(req);
    return NULL;
}

static int info_commom_back(LwqqHttpRequest* req,void* data)
{
    json_t* root=NULL;
    int errno = 0;
    int ret;

    if(req->http_code!=200){
        errno = LWQQ_EC_HTTP_ERROR;
        goto done;
    }
    lwqq_puts(req->response);
    ret = json_parse_document(&root,req->response);
    if(ret!=JSON_OK){
        errno = LWQQ_EC_ERROR;
        goto done;
    }
    const char* retcode = json_parse_simple_value(root,"retcode");
    if(retcode==NULL){
        errno = 1;
        goto done;
    }
    errno = s_atoi(retcode,1);
    void** d = data;
    long type = data ? (long)d[0] : 0;
    if(errno==0&&data!=NULL){
        switch(type){
            case CHANGE_BUDDY_MARKNAME:
                {
                    LwqqBuddy* buddy = d[1];
                    char* alias = d[2];
                    s_free(buddy->markname);
                    buddy->markname = alias;
                } break;
            case CHANGE_GROUP_MARKNAME:
                {
                    LwqqGroup* group = d[1];
                    char* alias = d[2];
                    s_free(group->markname);
                    group->markname = alias;
                } break;
            case MODIFY_BUDDY_CATEGORY:
                {
                    LwqqBuddy* buddy = d[1];
                    long cate_idx = (long)d[2];
                    buddy->cate_index = cate_idx;
                } break;
            case CHANGE_STATUS:
                {
                    LwqqClient* lc = d[1];
                    const char* status = d[2];
                    lc->stat = lwqq_status_from_str(status);
                    lc->status = lwqq_status_to_str(lc->stat);
                } break;
            case CHANGE_MASK:
                {
                    LwqqGroup* group = d[1];
                    LwqqMask mask = (LwqqMask)d[2];
                    group->mask = mask;
                } break;
        }
    }else if(errno == 108){
        switch(type){
            case CHANGE_STATUS:
                {
                    LwqqClient* lc = d[1];
                    lc->dispatch(vp_func_p,(CALLBACK_FUNC)lc->async_opt->poll_lost,lc);
                } break;
        }
    }
done:
    s_free(data);
    if(root)
        json_free_value(&root);
    lwqq_http_request_free(req);
    return errno;
}


LwqqAsyncEvent* lwqq_info_change_group_markname(LwqqClient* lc,LwqqGroup* group,const char* alias)
{
    if(!lc||!group||!alias) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/update_group_info2","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"r={\"gcode\":%s,\"markname\":\"%s\",\"vfwebqq\":\"%s\"}",
            group->code,alias,lc->vfwebqq
            );
    lwqq_puts(post);
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    void** data = s_malloc0(sizeof(void*)*3);
    data[0] = (void*)CHANGE_GROUP_MARKNAME;
    data[1] = group;
    data[2] = s_strdup(alias);
    return req->do_request_async(req,1,post,_C_(2p_i,info_commom_back,req,data));
done:
    lwqq_http_request_free(req);
    return NULL;
}

LwqqAsyncEvent* lwqq_info_change_discu_topic(LwqqClient* lc,LwqqGroup* group,const char* alias)
{
    if(!lc || !group || !alias) return NULL;
    char url[256];
    char post[512];
    snprintf(url,sizeof(url),"http://d.web2.qq.com/channel/modify_discu_info");
    snprintf(post,sizeof(post),"r={\"did\":\"%s\",\"discu_name\":\"%s\","
            "\"dtype\":1,\"clientid\":\"%s\",\"psessionid\":\"%s\",\"vfwebqq\":\"%s\"}",
            group->did,alias,lc->clientid,lc->psessionid,lc->vfwebqq);
    lwqq_puts(post);
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    req->set_header(req,"Origin","http://d.web2.qq.com");
    req->set_header(req,"Referer","http://d.web2.qq.com/proxy.html?v=20110331002&id=2");
    void **data = s_malloc(sizeof(void*)*3);
    data[0] = (void*)CHANGE_GROUP_MARKNAME;
    data[1] = group;
    data[2] = s_strdup(alias);
    return req->do_request_async(req,1,post,_C_(2p_i,info_commom_back,req,data));
}


LwqqAsyncEvent* lwqq_info_modify_buddy_category(LwqqClient* lc,LwqqBuddy* buddy,const char* new_cate)
{
    if(!lc||!buddy||!new_cate) return NULL;
    long cate_idx = -1;
    LwqqFriendCategory *c;
    LIST_FOREACH(c,&lc->categories,entries){
        if(strcmp(c->name,new_cate)==0){
            cate_idx = c->index;
            break;
        }
    }
    if(cate_idx==-1) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/modify_friend_group","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"tuin=%s&newid=%ld&vfwebqq=%s",
            buddy->uin,cate_idx,lc->vfwebqq );
    lwqq_puts(post);
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    void** data = s_malloc0(sizeof(void*)*3);
    data[0] = (void*)MODIFY_BUDDY_CATEGORY;
    data[1] = buddy;
    data[2] = (void*)cate_idx;
    return req->do_request_async(req,1,post,_C_(2p_i,info_commom_back,req,data));
done:
    lwqq_http_request_free(req);
    return NULL;
}

LwqqAsyncEvent* lwqq_info_delete_friend(LwqqClient* lc,LwqqBuddy* buddy,LWQQ_DEL_FRIEND_TYPE del_type)
{
    if(!lc||!buddy) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/delete_friend","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"tuin=%s&delType=%d&vfwebqq=%s",
            buddy->uin,del_type,lc->vfwebqq );
    lwqq_puts(post);
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    return req->do_request_async(req,1,post,_C_(2p_i,info_commom_back,req,NULL));
done:
    lwqq_http_request_free(req);
    return NULL;
}

//account is qqnumber
LwqqAsyncEvent* lwqq_info_allow_added_request(LwqqClient* lc,const char* account)
{
    if(!lc||!account) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/allow_added_request2","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"r={\"account\":%s,\"vfwebqq\":\"%s\"}",
            account,lc->vfwebqq );
    lwqq_puts(post);
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    return req->do_request_async(req,1,post,_C_(2p_i,info_commom_back,NULL));
done:
    lwqq_http_request_free(req);
    return NULL;
}

LwqqAsyncEvent* lwqq_info_deny_added_request(LwqqClient* lc,const char* account,const char* reason)
{
    if(!lc||!account) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/deny_added_request2","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"r={\"account\":%s,\"vfwebqq\":\"%s\"",
            account,lc->vfwebqq );
    if(reason){
        format_append(post,",\"msg\":\"%s\"",reason);
    }
    format_append(post,"}");
    lwqq_puts(post);
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    return req->do_request_async(req,1,post,_C_(2p_i,info_commom_back,req,NULL));
done:
    lwqq_http_request_free(req);
    return NULL;
}
LwqqAsyncEvent* lwqq_info_allow_and_add(LwqqClient* lc,const char* account,const char* markname)
{
    if(!lc||!account) return NULL;
    char url[512];
    char post[256];
    snprintf(url,sizeof(url),"%s/api/allow_and_add2","http://s.web2.qq.com");
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    if(req==NULL){
        goto done;
    }
    snprintf(post,sizeof(post),"r={\"account\":%s,\"gid\":0,\"vfwebqq\":\"%s\"",
            account,lc->vfwebqq );
    if(markname){
        format_append(post,",\"mname\":\"%s\"",markname);
    }
    format_append(post,"}");
    lwqq_puts(post);
    req->set_header(req,"Origin","http://s.web2.qq.com");
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&callback=0&id=3");
    return req->do_request_async(req,1,post,_C_(2p_i,info_commom_back,req,NULL));
done:
    lwqq_http_request_free(req);
    return NULL;
}

void lwqq_info_get_group_sig(LwqqClient* lc,LwqqGroup* group,const char* to_uin)
{
    if(!lc||!group||!to_uin) return;
    if(group->group_sig) return;
    LwqqSimpleBuddy* sb = lwqq_group_find_group_member_by_uin(group,to_uin);
    if(sb==NULL) return;
    char url[512];
    int ret;
    snprintf(url,sizeof(url),"%s/channel/get_c2cmsg_sig2?"
            "id=%s&"
            "to_uin=%s&"
            "service_type=0&"
            "clientid=%s&"
            "psessionid=%s&"
            "t=%ld",
            "http://d.web2.qq.com",
            group->gid,to_uin,lc->clientid,lc->psessionid,time(NULL));
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    if(req==NULL){
        goto done;
    }
    req->set_header(req, "Cookie", lwqq_get_cookies(lc));
    req->set_header(req,"Referer","http://d.web2.qq.com/proxy.html?v=20010321002&callback=0&id=2");
    ret = req->do_request(req,0,NULL);
    if(req->http_code!=200){
        goto done;
    }
    json_t* root = NULL;
    ret = json_parse_document(&root,req->response);
    if(ret!=JSON_OK){
        goto done;
    }
    sb->group_sig = s_strdup(json_parse_simple_value(root,"value"));

done:
    if(root)
        json_free_value(&root);
    lwqq_http_request_free(req);
}

LwqqAsyncEvent* lwqq_info_change_status(LwqqClient* lc,LwqqStatus status)
{
    if(!lc||!status) return NULL;
    char url[512];
    snprintf(url,sizeof(url),"%s/channel/change_status2?"
            "newstatus=%s&"
            "clientid=%s&"
            "psessionid=%s&"
            "t=%ld",
            "http://d.web2.qq.com",lwqq_status_to_str(status),lc->clientid,lc->psessionid,time(NULL));
    lwqq_puts(url);
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    req->set_header(req, "Cookie", lwqq_get_cookies(lc));
    req->set_header(req,"Referer","http://d.web2.qq.com/proxy.html?v=20110331002&id=3");
    void **data = s_malloc(sizeof(void*)*3);
    data[0] = (void*)CHANGE_STATUS;
    data[1] = lc;
    data[2] = (char*)lwqq_status_to_str(status);
    return req->do_request_async(req,0,NULL,_C_(2p_i,info_commom_back,req,data));
}
LwqqAsyncEvent* lwqq_info_search_friend_by_qq(LwqqClient* lc,const char* qq,LwqqBuddy* out)
{
    if(!lc||!qq||!out) return NULL;

    s_free(out->qqnumber);
    out->qqnumber = s_strdup(qq);
    LwqqAsyncEvent* ev = lwqq_async_event_new(NULL);
    ev->lc = lc;
    LwqqVerifyCode* c = s_malloc0(sizeof(*c));
    c->cmd = _C_(3p,add_friend_stage_2,ev,c,out);
    lwqq_util_request_captcha(lc, c);
    return ev;
}
static void add_friend_stage_2(LwqqAsyncEvent* called,LwqqVerifyCode* code,LwqqBuddy* out)
{
    if(!code->str){
        called->result = LWQQ_EC_ERROR;
        lwqq_async_event_finish(called);
        goto done;
    }
    char url[512];
    LwqqClient* lc = called->lc;
    snprintf(url,sizeof(url),"http://s.web2.qq.com/api/search_qq_by_uin2?tuin=%s&verifysession=%s&code=%s&vfwebqq=%s&t=%ld",
            out->qqnumber,lc->cookies->verifysession,code->str,lc->vfwebqq,time(NULL));
    lwqq_puts(url);
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    req->set_header(req,"Cookie",lwqq_get_cookies(lc));
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&id=1");
    req->set_header(req,"Connection","keep-alive");
    LwqqAsyncEvent* ev = req->do_request_async(req,0,NULL,_C_(2p_i,get_friend_detail_back,req,out));
    lwqq_async_add_event_chain(ev, called);
done:
    lwqq_vc_free(code);
}
static int get_friend_detail_back(LwqqHttpRequest* req,LwqqBuddy* out)
{
    int err = 0;
    if(req->http_code!=200){
        err = LWQQ_EC_ERROR;
        goto done;
    }
    //lwqq_puts(req->response);
    //{"retcode":0,"result":{"face":567,"birthday":{"month":x,"year":xxxx,"day":xxxx},"occupation":"","phone":"","allow":1,"college":"","constel":5,"blood":0,"stat":20,"homepage":"","country":"中国","city":"xx","uiuin":"","personal":"","nick":"xxx","shengxiao":x,"email":"","token":"523678fd7cff12223ef9906a4e924a4bf733108b9532c27c","province":"xx","account":xxx,"gender":"male","tuin":3202680423,"mobile":""}}
    json_t *root = NULL,*json_tmp;
    if(json_parse_document(&root, req->response)!=JSON_OK){
        lwqq_log(LOG_ERROR, "Parse json object of add friend error: %s\n", req->response);
        err = LWQQ_EC_ERROR;
        goto done;
    }
    int retcode = s_atoi(json_parse_simple_value(root, "retcode"),LWQQ_EC_ERROR);
    //WEBQQ_FATAL:验证码输入错误.
    if(retcode != WEBQQ_OK){
        err = retcode;
        goto done;
    }
    json_tmp = json_find_first_label(root, "result");
    if(json_tmp != NULL){
        json_tmp = json_tmp->child;
        parse_friend_detail_by_json(out,json_tmp);
    }

done:
    if(root){
        json_free_value(&root);
    }
    lwqq_http_request_free(req);
    return err;
}
LwqqAsyncEvent* lwqq_info_add_friend(LwqqClient* lc,LwqqBuddy* buddy,const char* message)
{
    if(!lc||!buddy) return NULL;
    if(!buddy->token){
        return NULL;
    }
    if(message==NULL)message="";

    char url[512];
    snprintf(url,sizeof(url),"http://s.web2.qq.com/api/add_need_verify2");
    char post[1024];
    //r:{"account":291205909,"myallow":1,"groupid":0,"msg":"xxx","token":"0a74690f4e7fb3df33de80b679515306f8def8cf7987251a","vfwebqq":"c674f106453f333320cd04a6499123807c7fc25137eac4137f564bdbe516b5ecfe143b8969707d30"}
    snprintf(post,sizeof(post),"r={\"account\":%s,\"myallow\":1,"
            "\"groupid\":%d,\"msg\":\"%s\","
            "\"token\":\"%s\",\"vfwebqq\":\"%s\"}",buddy->qqnumber,buddy->cate_index,message,buddy->token,lc->vfwebqq);
    lwqq_puts(post);
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    req->set_header(req,"Cookie",lwqq_get_cookies(lc));
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&id=1");
    return req->do_request_async(req,1,post,_C_(p_i,process_result_default,req));
}
static int process_result_default(LwqqHttpRequest* req)
{
    //{"retcode":0,"result":{"ret":0}}
    int err = 0;
    if(req->http_code!=200){
        err = LWQQ_EC_ERROR;
        goto done;
    }
    lwqq_puts(req->response);
    json_t *root = NULL;
    if(json_parse_document(&root, req->response)!=JSON_OK){
        lwqq_log(LOG_ERROR, "Parse json object of add friend error: %s\n", req->response);
        err = LWQQ_EC_ERROR;
        goto done;
    }
    int retcode = s_atoi(json_parse_simple_value(root, "retcode"),LWQQ_EC_ERROR);
    if(retcode != WEBQQ_OK){
        err = retcode;
    }
done:
    if(root) json_free_value(&root);
    lwqq_http_request_free(req);
    return err;
}

LwqqAsyncEvent* lwqq_info_search_group_by_qq(LwqqClient* lc,const char* qq,LwqqGroup* group)
{
    if(!lc||!qq||!group) return NULL;

    s_free(group->qq);
    group->qq = s_strdup(qq);
    LwqqAsyncEvent* ev = lwqq_async_event_new(NULL);
    ev->lc = lc;
    LwqqVerifyCode* code = s_malloc0(sizeof(*code));
    code->cmd = _C_(3p,add_group_stage_1,ev,code,group);
    lwqq_util_request_captcha(lc, code);
    return ev;
}

static void add_group_stage_1(LwqqAsyncEvent* called,LwqqVerifyCode* code,LwqqGroup* g)
{
    if(!code->str) goto done;
    char url[512];
    LwqqClient* lc = called->lc;
    snprintf(url,sizeof(url),
            "http://cgi.web2.qq.com/keycgi/qqweb/group/search.do?"
            "pg=1&perpage=10&all=%s&c1=0&c2=0&c3=0&st=0&vfcode=%s&type=1&vfwebqq=%s&t=%ld",
            g->qq,code->str,lc->vfwebqq,time(NULL));
    lwqq_puts(url);
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    req->set_header(req,"Cookie",lwqq_get_cookies(lc));
    req->set_header(req,"Referer","http://cgi.web2.qq.com/proxy.html?v=20110412001&id=1");
    LwqqAsyncEvent* ev = req->do_request_async(req,0,NULL,_C_(2p_i,add_group_stage_2,req,g));
    lwqq_async_add_event_chain(ev, called);
done:
    lwqq_vc_free(code);
}

static int add_group_stage_2(LwqqHttpRequest* req,LwqqGroup* g)
{
    //{"result":[{"GC":"","GD":"","GE":3090888,"GF":"","TI":"永远的电子商务033班","GA":"","BU":"","GB":"","DT":"","RQ":"","QQ":"","MD":"","TA":"","HF":"","UR":"","HD":"","HE":"","HB":"","HC":"","HA":"","LEVEL":0,"PD":"","TX":"","PA":"","PB":"","CL":"","GEX":1013409473,"PC":""}],"retcode":0,"responseHeader":{"CostTime":18,"Status":0,"TotalNum":1,"CurrentNum":1,"CurrentPage":1}}
    int err = 0;
    lwqq_util_valid_http_request(req);
    json_t* root = NULL,*result;
    lwqq_util_valid_and_parse_json(root,req->response);
    int retcode = lwqq_util_json_get_retcode(root);
    if(retcode != WEBQQ_OK) {err = LWQQ_EC_ERROR;goto done;}
    result = json_find_first_label(root, "result");
    if(result && result->child && result->child->child){
        result = result->child->child;
        g->name = json_unescape(json_parse_simple_value(result, "TI"));
        g->code = s_strdup(json_parse_simple_value(result, "GEX"));
    }else{
        err = LWQQ_EC_NO_RESULT;
    }
done:
    if(root) json_free_value(&root);
    lwqq_http_request_free(req);
    return err;
}

LwqqAsyncEvent* lwqq_info_add_group(LwqqClient* lc,LwqqGroup* g,const char* msg)
{
    if(!lc||!g) return NULL;
    if(msg==NULL) msg = "";
    LwqqAsyncEvent* ev = lwqq_async_event_new(NULL);
    ev->lc = lc;
    LwqqVerifyCode* c = s_malloc0(sizeof(*c));
    c->cmd = _C_(3p,add_group_stage_4,ev,c,g);
    lwqq_util_request_captcha(lc, c);
    return ev;
}

static void add_group_stage_4(LwqqAsyncEvent* called,LwqqVerifyCode* c,LwqqGroup* g)
{
    if(c->str==NULL){
        called->result = LWQQ_EC_ERROR;
        lwqq_async_event_finish(called);
        goto done;
    }
    char url[256];
    char post[512];
    LwqqClient* lc = called->lc;
    snprintf(url,sizeof(url),"http://s.web2.qq.com/api/apply_join_group2");
    snprintf(post,sizeof(post),"r={\"gcode\":%s,\"code\":\"%s\",\"vfy\":\"%s\","
            "\"msg\":\"%s\",\"vfwebqq\":\"%s\"}",
            g->code,c->str,lc->cookies->verifysession,"",lc->vfwebqq);
    lwqq_puts(post);
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc, url, NULL);
    req->set_header(req,"Cookie",lwqq_get_cookies(lc));
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&id=3");
    LwqqAsyncEvent* ev = req->do_request_async(req,1,post,_C_(p_i,process_result_default,req));
    lwqq_async_add_event_chain(ev, called);
done:
    lwqq_vc_free(c);
}


LwqqAsyncEvent* lwqq_info_mask_group(LwqqClient* lc,LwqqGroup* group,LwqqMask mask)
{
    if(!lc||!group) return NULL;
    char url[512];
    char post[512];
    snprintf(url,sizeof(url),"http://cgi.web2.qq.com/keycgi/qqweb/uac/messagefilter.do");
    lwqq_puts(url);
    const char* mask_type;

    mask_type = (group->type == LWQQ_GROUP_QUN)? "groupmask":"discumask";

    snprintf(post,sizeof(post),"retype=1&app=EQQ&itemlist={\"%s\":{"
            "\"%s\":\"%d\",\"cAll\":0,\"idx\":%s,\"port\":%s}}&vfwebqq=%s",
            mask_type,group->gid,mask,lc->index,lc->port,lc->vfwebqq);
    lwqq_puts(post);
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc,url,NULL);
    req->set_header(req, "Cookie", lwqq_get_cookies(lc));
    req->set_header(req,"Referer","http://cgi.web2.qq.com/proxy.html?v=20110412001&id=2");
    void **data = s_malloc(sizeof(void*)*3);
    data[0] = (void*)CHANGE_MASK;
    data[1] = group;
    data[2] = (void*)mask;
    return req->do_request_async(req,1,post,_C_(2p_i,info_commom_back,req,data));
}

LwqqAsyncEvent* lwqq_info_get_stranger_info(LwqqClient* lc,LwqqMsgSysGMsg* msg,LwqqBuddy* buddy)
{
    if(!lc||!msg||!buddy) return NULL;
    char url[512];
    snprintf(url,sizeof(url),"http://s.web2.qq.com/api/get_stranger_info2?tuin=%s&verifysession=&gid=0&code=%s-%s&vfwebqq=%s&t=%ld",msg->request_join.request_uin,"group_request_join",msg->group_uin,lc->vfwebqq,time(NULL));
    lwqq_puts(url);
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc, url, NULL);
    req->set_header(req,"Cookie",lwqq_get_cookies(lc));
    req->set_header(req,"Referer","http://s.web2.qq.com/proxy.html?v=20110412001&id=3");
    return req->do_request_async(req,0,NULL,_C_(2p,get_friend_detail_back,req,buddy));
}

LwqqAsyncEvent* lwqq_info_answer_request_join_group(LwqqClient* lc,LwqqMsgSysGMsg* msg ,LwqqAnswer answer,const char* reason)
{
    if(!lc||!msg) return NULL;
    if(reason==NULL) reason="";
    char url[512];
    int op = (answer)?2:3;
    snprintf(url,sizeof(url),"http://d.web2.qq.com/channel/op_group_join_req?"
            "group_uin=%s&req_uin=%s&msg=%s&op_type=%d&clientid=%s&psessionid=%s&t=%ld",
            msg->group_uin,msg->request_join.request_uin,reason,op,lc->clientid,lc->psessionid,time(NULL));
    LwqqHttpRequest* req = lwqq_http_create_default_request(lc, url, NULL);
    req->set_header(req,"Cookie",lwqq_get_cookies(lc));
    req->set_header(req,"Referer","http://d.web2.qq.com/proxy.html?v=20110331002&id=2");
    return req->do_request_async(req,0,NULL,_C_(p_i,process_result_default,req));
}
