/*
@file: hwlog_new.c
@brief:Ĭ���Ѽ�ERROR��־,���浽flash,ͬʱ��������·���־�Ѽ�,
       ���Զ��л��������·�����־����,�����ռ�������־���ļ�����ʽ�ϴ�
@�������˼·:
  1.����swsyslog ģ��ʵ����־���Ѽ�,�ڱ�ģ��ʵ����־������,�л�,���,�Լ��ϴ�
  2.��־�ռ�ģ�������Ϊ��һ���߳�,�����·�������һ���߳�,��ģ����һ���߳�

*/


#include "swapi.h"
#include "swlog.h"
#include "swurl.h"
#include "swparameter.h"
#include "hwlog.h"
#include "hwiptv.h"
#include "hwiptv_priv.h"
#include "swthrd.h"
#include <dirent.h>
#include "hwtime.h"
#include "hwiptv_priv.h"
#include "hwlog_new.h"
#include "hwview.h"
#include "hwtime.h"
#include "hwdevcert.h"
#include "swbrowser.h"

#ifdef SUPPORT_HTTPS
#include "swhttpsclient.h"
#else
#include "swhttpclient.h"
#endif

#define STB_LOG_PATH "/var/log"                  //������ռ�����־
#define TMP_LOG_BASE_PATH  "/tmp/log"           //��ʱ��־��log
#define TMP_LOG_PATH      "/tmp/log/logtmp"    //��־��ʱĿ¼
#define MAX_LOG_NUM   5                   //��ౣ��5��log

#define ERROR_LOG  1 //error��־
#define DEBUG_LOG  2 //debug��־

extern int swsyscmd(const char* cmd );
extern bool sw_get_dvb_sync_time_state(void); //DVBͬ��ʱ��

typedef struct _level_info
{
	int level;
	unsigned int time_out; //��
	unsigned int start_time; //��
}level_info_t;

typedef enum
{
	TMS_LOG_OFF = 0x0,
	TMS_LOG_ERROR,
	TMS_LOG_INFO,
	TMS_LOG_DEBUG,
}tms_log_level_t;

typedef enum
{
	LOG_MODE_NULL,
	LOG_MODE_ERROR,
	LOG_MODE_DEBUG,
	LOG_MODE_USB,
}log_mode_t;

static int m_start_time = 0; //��ʼץ��־��ʱ��
static int m_end_time = 0; //����ץ��־��ʱ��
static int m_log_level = LOG_LEVEL_OFF; //ץ��־�ļ���
static char m_mac[24];
static char m_stb_type[16];

static int m_default_level = LOG_LEVEL_ERROR;

static int m_log_mode = LOG_MODE_NULL; //��ǰ�̵߳�״̬ 

HANDLE m_tmslogrun_thrd = NULL; //��־�ռ�,��־���,��־�ϴ��߳�

static level_info_t m_chang_info;
static HANDLE m_colloectlog_thrd = NULL;
static bool m_proc_exit = false;

static int m_commit_time = 0; //���һ�δ����־��ʱ��

static sw_url_t m_server_url;              //������server
static int m_time_out = 12000;             //��ʱʱ��

static int m_interval_debug = 10*60; //Ĭ��10����,��λ����
static int m_interval_error = 2*60*60*1000; //Ĭ��2Сʱ
static int m_max_size = 3*1024*1024; //3M

static bool m_usb_is_collect = false; //U��ץ��־�ı�־

//=========�ļ��ϴ�=========

//�����ϴ���־������,˫����֤��sw_httpsclient_connect_verify�ӿ���ʵ��
static HANDLE connect_to_logserver(void)
{
	HANDLE client = NULL;
	char log_server[64] = {0};
	int connect_count = 0;
	int time_cnt = 1;
	hw_newlog_tms_get_para("LogServer",log_server,sizeof(log_server));
	HWIPTV_LOG_DEBUG("log_server = %s\n",log_server);
	if(log_server[0] == '\0')
		return NULL;
	char *server_type = NULL;
	void *key = NULL;
#ifdef SUPPORT_HTTPS
	server_type = "https://";
#else
	server_type = "http://";
#endif
	if(strncmp(log_server,server_type,strlen(server_type) != 0))
	{
		HWIPTV_LOG_ERROR("log_server is %s\n",log_server);
		return NULL ;
	}
	sw_memset(&m_server_url,sizeof(m_server_url),0,sizeof(m_server_url));
	if(sw_url_parse(&m_server_url, log_server) != 0)
	{
		HWIPTV_LOG_ERROR("fail to parse log_server is %s\n",log_server);
		return NULL;
	}
	if( m_server_url.ip == 0 || m_server_url.port == 0 )
	{
		HWIPTV_LOG_ERROR("m_server_url.ip == %u, m_server_url.port == %d\n",m_server_url.ip,m_server_url.port);
		return NULL;
	}

	for(connect_count = 0;connect_count < 3;connect_count++) //�����3������
	{
#ifdef SUPPORT_HTTPS
#ifdef SUPPORT_MKDEVCERT_ONLINE
		key = sw_dev_private_key_get();
#else
		key = NULL;
#endif
		client = sw_httpsclient_connect_verify( m_server_url.ip, m_server_url.port, m_time_out,key );
#else
		client = sw_httpclient_connect(m_server_url.ip, m_server_url.port, m_time_out);
#endif
		if(client != NULL)
			break;
		else
		{ //����������֤���飬���ʧ�ܳ���3������ʱ���� 0,5,10
			sw_thrd_delay(time_cnt*5*1000);
			time_cnt ++;
		}
	}
	return client;
}

//���������ļ���������
static bool send_file_to_server(HANDLE *client,char *file_name,char *s_file_name)
{
	if(client == NULL || file_name == NULL)
		return false;
	if(file_name[0] == '\0')
		return false;
	//POST
	bool ret = false;
	int ret_size = 0;
	struct stat file_stat;
	sw_memset(&file_stat,sizeof(file_stat),0,sizeof(file_stat));
	if(lstat(file_name,&file_stat) != 0)
		return false;
	int pbuf_size = (file_stat.st_size/8 + 1)*8; //8�ֽڶ���
	char *pbuf = (char*) malloc(pbuf_size);
	if(pbuf == NULL)
	{
		HWIPTV_LOG_ERROR("fail to malloc\n");
		return false;
	}
	sw_memset(pbuf,pbuf_size,0,pbuf_size);
	//read log
	FILE *file_fp = fopen(file_name,"r");
	if(file_fp == NULL)
	{
		HWIPTV_LOG_ERROR("fail to open file %s\n",file_name);
		goto ERROR;
	}
	ret_size = fread(pbuf,sizeof(char),file_stat.st_size,file_fp);
	if(ret_size != file_stat.st_size)
	{
		HWIPTV_LOG_ERROR("fail to read file %s,ret_size = %d\n",file_name,ret_size);
		goto ERROR;
	}
#ifdef SUPPORT_HTTPS
	ret = sw_httpsclient_request_ex( client, "POST", m_server_url.path,m_server_url.hostname, NULL, s_file_name,"file/gz", pbuf, ret_size, m_time_out, "", NULL );
#else
	ret = sw_httpclient_request_ex2( client, "POST", m_server_url.path,m_server_url.hostname, NULL, s_file_name,"file/gz", pbuf, ret_size, m_time_out, "", NULL );
#endif
	if(ret == false)
	{
		HWIPTV_LOG_ERROR("fail to POST file %s ,remote patch = %s,hostname = %s\n",file_name,m_server_url.path,m_server_url.hostname);
		goto ERROR;
	}
	//���ͳɹ����������
	sw_memset(pbuf,pbuf_size,0,pbuf_size);
	//recv
#ifdef SUPPORT_HTTPS
	ret_size = sw_httpsclient_recv_data( client, pbuf,pbuf_size,m_time_out);
#else
	ret_size = sw_httpclient_recv_data( client, pbuf,pbuf_size,m_time_out);
#endif
	HWIPTV_LOG_DEBUG("recv_buf = %s\n",pbuf);
	if(strstr( pbuf, "HTTP/1.1 200" ) != NULL) //�������������
		ret = true;
	else
		ret = false;
ERROR:
	if(pbuf)
		free(pbuf);
	if(file_fp)
		fclose(file_fp);
	pbuf = NULL;
	file_fp = NULL;
	return ret;
}

static void disconnect_from_server(HANDLE *client)
{
	if(client == NULL)
		return;
#ifdef SUPPORT_HTTPS
	sw_httpsclient_disconnect(client);
#else
	sw_httpclient_disconnect(client);
#endif
	return;
}

static void get_server_file_name(char *file_name,int size,int type)
{
	struct tm tm;
	time_t now = 0;
	char tmp_time[16] = {0};
	char stb_id[24] = {0};
	if(!sw_parameter_get("serial",stb_id,sizeof(stb_id)))
	{
		HWIPTV_LOG_ERROR("fail to get STB ID \n");
	}
	sw_memset(&tm,sizeof(tm),0,sizeof(tm));
	now = time(NULL);
	localtime_r(&now, &tm);
	strftime(tmp_time,sizeof(tmp_time),"%Y%m%d%H%M%S", &tm);
	if(type == ERROR_LOG)
		sw_snprintf(file_name,size,0,size,"%s_%s_error.gz",stb_id,tmp_time);
	else
		sw_snprintf(file_name,size,0,size,"%s_%s.gz",stb_id,tmp_time);
	return;
}

//��־�ϴ��ӿڣ��������ⲿʵ�֣����سɹ��ϴ��ļ��ĸ���
static bool log_file_upload(char *filename)
{
	//��ȷ���Ƿ����ļ�
	//��Ҫ��/var/log/�е���־ȫ���ϴ�
	bool send_ret = true;
	int ret = 0;
	char s_file_name[64] = {0}; //�������б��������
	char full_name[64] = {0};
	DIR *dirp = NULL;
	struct dirent dirbuf;
	struct dirent *presult = NULL;
	int name_len = 0;
	HANDLE client = NULL;
	int type = 0;

	unsigned int tmp_time = 0;

	sw_snprintf(s_file_name,sizeof(s_file_name),0,sizeof(s_file_name),"errorlog_%s_MMddHHmmss_FFFFFFFFFFFF.tgz",m_stb_type);
	name_len = strlen(s_file_name); //������־����������һ����

	dirp = opendir(STB_LOG_PATH);
	if(dirp == NULL)
	{
		HWIPTV_LOG_ERROR("fail to open log dir\n");
		goto TMP_FILE;
	}
	//�ȼ��falsh���Ƿ���Ҫ�ϴ����ļ�
	for( ; ; )
	{
		ret = readdir_r(dirp,&dirbuf,&presult);
		if(ret != 0 || presult == NULL)
		{
			HWIPTV_LOG_INFO("finish to read dir or read error ret = %d,presult=%p\n",ret,presult);
			break;
		}
		HWIPTV_LOG_DEBUG("dirbuf.d_name = %s,name_len = %d\n",dirbuf.d_name,name_len);
		//����flash�е�Ŀ¼.
		if(strlen(dirbuf.d_name) == name_len)
		{
			if(strncmp(dirbuf.d_name,"debuglog",strlen("debuglog")) ==0 )
				type = DEBUG_LOG;
			else if(strncmp(dirbuf.d_name,"errorlog",strlen("errorlog")) ==0 )
				type = ERROR_LOG;
			else //��������Ǽ���
				continue;
		}
		else
			continue;

		sw_snprintf(full_name,sizeof(full_name),0,sizeof(full_name),"%s/%s",STB_LOG_PATH,dirbuf.d_name);
		// ���ӷ�����
		if(client == NULL)
			client = connect_to_logserver(); //�ϴ�֮ǰ����connect,����ʵ��˫����֤
		if(client) //�ϴ�
		{
			//��װ��������Ҫ���������
			sw_memset(s_file_name,sizeof(s_file_name),0,sizeof(s_file_name));
			get_server_file_name(s_file_name,sizeof(s_file_name),type);
			HWIPTV_LOG_DEBUG("full_name = %s,s_file_name = %s\n",full_name,s_file_name);
			tmp_time = sw_thrd_get_tick();
			send_ret = send_file_to_server(client,full_name,s_file_name);
			if(send_ret) //ɾ���������ϴ����ļ�
				remove(full_name);
			else
			{
				HWIPTV_LOG_ERROR("fail to send log file\n");
				send_ret = false;
				break;
			}
		}
		else
		{
			HWIPTV_LOG_ERROR("fail to conect log server\n");
			send_ret = false;
			break;
		}
		tmp_time = sw_thrd_get_tick() - tmp_time;
		if(tmp_time < 1000) //��ֹ�ϴ�̫��,�ڷ������ϱ�����
			sw_thrd_delay(1000 - tmp_time);
		sw_memset(full_name,sizeof(full_name),0,sizeof(full_name));
	}
	closedir(dirp);
TMP_FILE:
	//�ϴ������/tmpĿ¼�е�debug��־
	if( send_ret && filename[0] != '\0' && client == NULL)
	{
		client = connect_to_logserver(); //�ϴ�֮ǰ����connect,����ʵ��˫����֤
		if(client)
		{
			sw_memset(full_name,sizeof(full_name),0,sizeof(full_name));
			sw_memset(s_file_name,sizeof(s_file_name),0,sizeof(s_file_name));
			get_server_file_name(s_file_name,sizeof(s_file_name),DEBUG_LOG); 
			sw_snprintf(full_name,sizeof(full_name),0,sizeof(full_name),"%s/%s",TMP_LOG_BASE_PATH,filename);
			HWIPTV_LOG_DEBUG("full_name = %s,s_file_name = %s\n",full_name,s_file_name);
			send_ret = send_file_to_server(client,full_name,s_file_name);
			if(send_ret) //�ϴ��ɹ�,ɾ�������ļ�
				remove(full_name);
		}
		else
			send_ret = false;
	}
	if(client)
		disconnect_from_server(client); //�ϴ������Ͽ�����
	client = NULL;
	return send_ret;
}

//========================��������======
//ͬʱ������˼��־
static int set_loglevel(int type)
{
	char cmd[40] = {0};
	int hisi_level = 1; //1 ERROR,2.WARA 3.INFO
	if(type == ERROR_LOG)
	{
		if(m_chang_info.level == LOG_LEVEL_OFF)
			sw_log_set_level(m_default_level);
		hw_view_show_debug_status(false);	
		sw_snprintf(cmd, sizeof(cmd), 0, sizeof(cmd), "echo hi_avplay=1 > /proc/msp/log");
		swsyscmd(cmd);
	}
	else if(type == DEBUG_LOG)
	{
		if(m_chang_info.level == LOG_LEVEL_OFF)
			sw_log_set_level(m_log_level);
		if(m_log_level == LOG_LEVEL_ALL)
		{
			sw_browser_debug(true);
			hisi_level = 1;
		}
		else if(m_log_level == LOG_LEVEL_INFO)
			hisi_level = 2;
		else if(m_log_level == LOG_LEVEL_ERROR) //���ں�˼��־��̫��
			hisi_level = 3;

		if(m_log_level == LOG_LEVEL_ALL)
			hw_view_show_debug_status(true);
		else
			hw_view_show_debug_status(false);

		sw_snprintf(cmd, sizeof(cmd), 0, sizeof(cmd), "echo hi_avplay=%d > /proc/msp/log",hisi_level);
		swsyscmd(cmd);
	}
	HWIPTV_LOG_DEBUG("set_log_level %d,hisi log level %d\n",sw_log_get_level(),hisi_level);
	return 0;
}

//���falsh���Ƿ񱣴���ָ�����ļ�,�������X����ɾ�������.
static int check_save_log(char *path, int type, int max_count)
{
	struct stat file;
	if(lstat(path,&file) != 0)
	{
		HWIPTV_LOG_ERROR("%s is not exist\n",path);
		return -1;
	}
	int i = 0;
	char filename[max_count][128];
	//ȥ��������Ƿ���־
	DIR *dirp = NULL;
	struct dirent dirbuf;
	struct dirent *presult = NULL;
	int ret = 0;
	int count = 0;
	int old_index = 0;
	char *info = NULL;
	int name_len = 0;
	for(i=0; i<max_count; i++)
		sw_memset(filename[i],sizeof(filename[i]),0,sizeof(filename[i]));

	name_len = strlen("errorlog_MMddHHmmss_FFFFFFFFFFFF.tgz");
	if(type == ERROR_LOG)
		info = "errorlog";
	else
		info = "debuglog";

	dirp = opendir(path);
	if(dirp == NULL)
	{
		HWIPTV_LOG_ERROR("fail to open log dir\n");
		return -2;
	}
	while(1)
	{
		ret = readdir_r(dirp,&dirbuf,&presult);
		if(ret != 0 || presult == NULL)
		{
			HWIPTV_LOG_DEBUG("finish to read dir or read error ret = %d,presult=%p\n",ret,presult);
			break;
		}
		if(strncmp(dirbuf.d_name,info,strlen(info)) !=0 || strlen(dirbuf.d_name) != name_len ) //����������Ҫ����ļ�
			continue;

		if(count < max_count)
			sw_snprintf(filename[count],sizeof(filename[count]),0,sizeof(filename[count]),"%s",dirbuf.d_name);
		if(count < max_count)
			HWIPTV_LOG_DEBUG("filename[%d] = %s\n",count,filename[count]);

		count++;
	}
	closedir(dirp);
	//�Ƴ����µ��Ǹ��ļ� error_201807101234.tbz,�����ַ����Ƚ�ȡ����ɵ��Ǹ��ļ���
	if(count >= max_count)
	{
		for(i=1;i<max_count;i++)
		{
			if(strncasecmp(filename[i],filename[old_index],name_len)<0)
				old_index = i;
		}
		//ɾ���ļ�
		char cmd[256] = {0};
		sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"%s/%s",path,filename[old_index]);
		HWIPTV_LOG_DEBUG("remove file %s,old_index = %d\n",cmd,old_index);
		remove(cmd);
		count --;
	}
	return count; //����Ŀ¼���ж��ٸ��ļ�
}
static void get_time_info(char *file)
{
	struct tm tmp;
	char time_buf[24] = {0};
	char cmd[120] = {0};
	time_t now = time(NULL);
	localtime_r(&now, &tmp);
	strftime(time_buf,sizeof(time_buf),"%Y%m%d%H%M%S", &tmp);
	sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"echo -----collect time: %s------ >> %s/%s",time_buf,TMP_LOG_PATH,file);
	swsyscmd(cmd);
}

static void get_proc_info(void)
{
	char buf[160] = {0};
	char *proc_info[] = {"/proc/msp/adec00 /proc/msp/avplay00 /proc/msp/avplay01 /proc/msp/disp0 /proc/msp/disp1",
		                 "/proc/msp/hdmi0 /proc/msp/hdmi0_sink /proc/msp/sound0 /proc/msp/sync00 /proc/msp/sync01",
						 "/proc/msp/tuner /proc/msp/vpss00 /proc/msp/win0000 /proc/msp/win0100 /proc/media-mem",
						 "/proc/vfmw*"};
	int i = 0;
	get_time_info("StbProcinfo.txt");
	for(i=0;i < sizeof(proc_info)/sizeof(proc_info[0]);i++ )
	{
		sw_memset(buf,sizeof(buf),0,sizeof(buf));
		sw_snprintf(buf,sizeof(buf),0,sizeof(buf),"cat %s >> %s/StbProcinfo.txt",proc_info[i],TMP_LOG_PATH);
		swsyscmd(buf);
		HWIPTV_LOG_DEBUG("%s\n",buf);
	}
	return;
}

static void get_top_info(void)
{
	char buf[64] = {0};
	get_time_info("top.log");
	sw_snprintf(buf,sizeof(buf),0,sizeof(buf),"top -n 1 >> %s/top.log &",TMP_LOG_PATH);
	swsyscmd(buf);
	return;
}

//�ж���־�Ƿ�����һ����С,STBlog,proc,hisilog
static int get_collectlog_size(int type)
{
	int ret = 0;
	struct stat file;
	char file_name[64] = {0};

	sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));
	sw_memset(&file,sizeof(file),0,sizeof(file));

	if(type == ERROR_LOG) //todo error ��־ֻ��Ҫ���һ����־�ļ�����
		sw_snprintf(file_name,sizeof(file_name),0,sizeof(file_name),"%s/error0.log",TMP_LOG_PATH);
	else
		sw_snprintf(file_name,sizeof(file_name),0,sizeof(file_name),"%s/debug0.log",TMP_LOG_PATH);

	if(lstat(file_name,&file) == 0)
		ret += file.st_size;

	sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));
	sw_memset(&file,sizeof(file),0,sizeof(file));
	sw_snprintf(file_name,sizeof(file_name),0,sizeof(file_name),"%s/top.log",TMP_LOG_PATH);
	if(lstat(file_name,&file) == 0)
		ret += file.st_size;

	if(type == DEBUG_LOG) //tms debug��־����3����־,STB ,hisi,proc
	{
		sw_memset(&file,sizeof(file),0,sizeof(file));
		sw_snprintf(file_name,sizeof(file_name),0,sizeof(file_name),"%s/stb.log",TMP_LOG_PATH);
		if(lstat(file_name,&file) == 0)
		ret += file.st_size;

		sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));
		sw_memset(&file,sizeof(file),0,sizeof(file));
		sw_snprintf(file_name,sizeof(file_name),0,sizeof(file_name),"%s/StbProcinfo.txt",TMP_LOG_PATH);
		if(lstat(file_name,&file) == 0)
			ret += file.st_size;
	}
	return ret;
}

static void clean_log_targets(int type)
{
	char cmd[40] = {0};
	//����һ������׼��targets,��־���޷��������
	sw_log_set_targets("NULL");
	if(type == DEBUG_LOG)
	{
		sw_memset(cmd,sizeof(cmd),0,sizeof(cmd));
		sw_snprintf(cmd, sizeof(cmd), 0, sizeof(cmd), "echo log= > /proc/msp/log");
		swsyscmd(cmd);
	}
	return;
}
static void set_log_targets(int type)
{
	char cmd[128] = {0};
	struct stat path;
	//if(type == ERROR_LOG)
	if(lstat(TMP_LOG_PATH,&path) != 0)
		mkdir(TMP_LOG_PATH,600);
	if(type == ERROR_LOG)
	{
#ifdef SUPPORT_OPEN_STBMONITOR_SERIAL
		sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"file:///%s/error.log&max_size=4M,localsock"/*console*/,TMP_LOG_PATH);
#else
		sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"file:///%s/error.log&max_size=4M",TMP_LOG_PATH);
#endif
	}
	else
	{
#ifdef SUPPORT_OPEN_STBMONITOR_SERIAL
		 sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"file:///%s/debug.log&max_size=4M,localsock"/*,console"*/,TMP_LOG_PATH);
#else
		 sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"file:///%s/debug.log&max_size=4M",TMP_LOG_PATH);
#endif
	}
	sw_log_set_targets(cmd);

	sw_memset(cmd,sizeof(cmd),0,sizeof(cmd));
	if(type == DEBUG_LOG)
	{
		sw_snprintf(cmd, sizeof(cmd), 0, sizeof(cmd), "echo log=%s > /proc/msp/log",TMP_LOG_PATH);
		HWIPTV_LOG_DEBUG("cmd: %s\n",cmd);
		swsyscmd(cmd);
	}
	return;
}
//����ʱ�ļиĳ�Լ����ʽ�����
static int chang_tmp_log_path(int type,char *log_path,int size)
{
	struct tm tm;
	sw_memset(&tm,sizeof(tm),0,sizeof(tm));
	char file[64] = {0};
	char cmd[128] = {0}; 
	time_t now = time(NULL);
	if(localtime_r(&now, &tm)==NULL)
	{
		HWIPTV_LOG_ERROR("get localtime error\n");
		return -1;
	}
	if(type == ERROR_LOG)
		sw_snprintf(file,sizeof(file),0,sizeof(file),"errorlog_%s_%02d%02d%02d%02d%02d_%s",m_stb_type,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec,m_mac);
	else //debug
		sw_snprintf(file,sizeof(file),0,sizeof(file),"debuglog_%s_%02d%02d%02d%02d%02d_%s",m_stb_type,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec,m_mac);

	sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"mv %s %s/%s",TMP_LOG_PATH,TMP_LOG_BASE_PATH,file);
	swsyscmd(cmd); //�޸�����
	sw_snprintf(log_path,size,0,size,"%s",file);
	return 0;
}


//��Ŀ¼strpath ���������destpath,���ش������ļ���
static int pack_and_compress_log(int type,char *src_path,char *file_name,int size)
{
	char cmd[128] = {0}; 
	if(type == DEBUG_LOG) //���Ʋ���,Ƶ���б����
	{
		sw_memset(cmd,sizeof(cmd),0,sizeof(cmd));
		sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"cp /var/dvb/channellist.dat /var/.iptv/para /var/revise.history %s/%s -f",TMP_LOG_BASE_PATH,src_path);
		HWIPTV_LOG_DEBUG("cmd : %s\n",cmd);
		swsyscmd(cmd);
	}
	sw_memset(cmd,sizeof(cmd),0,sizeof(cmd));
	sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"busybox tar -czf %s/%s.tgz -C %s %s",TMP_LOG_BASE_PATH,src_path,TMP_LOG_BASE_PATH,src_path);
	HWIPTV_LOG_DEBUG("cmd : %s\n",cmd);
	swsyscmd(cmd); //���ѹ��
	//ɾ��ԭĿ¼
	sw_memset(cmd,sizeof(cmd),0,sizeof(cmd));
	sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"rm %s/%s -rf",TMP_LOG_BASE_PATH,src_path);
	HWIPTV_LOG_DEBUG("cmd : %s\n",cmd);
	swsyscmd(cmd); //���ѹ��
	sw_snprintf(file_name,size,0,size,"%s.tgz",src_path);
	return 1;
}

//��strptahĿ¼�µ�filename�ƶ���destpathĿ¼��,type˵�����ļ���ʲô�ļ�
static void save_file_to_flash(char *filename,int type)
{
	char cmd[256] = {0};
	int ret = 0;
	for(;;) //�ƶ�ǰ,�ȼ��Ŀ��Ŀ¼�Ƿ��Ѵ����ֵ
	{
		ret = check_save_log(STB_LOG_PATH,type,MAX_LOG_NUM);
		if(ret<MAX_LOG_NUM)
			break;
	}
	//mv /xxxxx/xxxxx.tgz /xxxx
	sw_snprintf(cmd,sizeof(cmd),0,sizeof(cmd),"mv %s/%s %s",TMP_LOG_BASE_PATH,filename,STB_LOG_PATH);
	HWIPTV_LOG_DEBUG("cmd : %s\n",cmd);
	swsyscmd(cmd);
	return;
}

//��ʱ�޸���־����,timeout,��ץ�ü����ץ���
void hw_set_tmp_loglevel(int level,int timeout)
{
	if(level > LOG_LEVEL_OFF || level < LOG_LEVEL_ALL)
	{
		HWIPTV_LOG_DEBUG("level illegal\n");
		return;
	}
	m_chang_info.level = level;
	m_chang_info.time_out=timeout;
	m_chang_info.start_time = sw_thrd_get_tick()/1000;
	HWIPTV_LOG_INFO("log_level %d ---> %d\n",sw_log_get_level(),level);
	sw_log_set_level(level);
}


//ͳһ������־(�ϴ��򱣴�)
static void commit_log_file(int type,char *filename)
{
	printf("commit file %s,type = %d\n",filename,type);
	int ret = 0;
	char save_name[64] = {0};
	if(filename[0] != '\0')
	{
		ret = pack_and_compress_log(type,filename,save_name,sizeof(save_name));
	}

	if(type == ERROR_LOG && ret > 0)
		save_file_to_flash(save_name,type);	
	else
		if(!log_file_upload(save_name) && ret > 0)
			save_file_to_flash(save_name,type);
	HWIPTV_LOG_DEBUG("finish  to commit log\n");
	return;
}


static int collect_log_check(int type)
{
	int tmp_time = 0;
	int collect_size = 0;
	int max_time = 0;
	if(type == ERROR_LOG)
		max_time = m_interval_error;
	else
		max_time = m_interval_debug;
		
	if(m_chang_info.time_out > 0) //Ԥ������,�л���־����һ��ʱ��
	{
		tmp_time = (sw_thrd_get_tick()/1000 - m_chang_info.start_time);

		if(tmp_time > m_chang_info.time_out) //�ָ���־
		{
			int log_level = 0;
			m_chang_info.time_out = 0;
			m_chang_info.level = LOG_LEVEL_OFF;
			if(type == ERROR_LOG)
				log_level = m_default_level;
			else
				log_level = m_log_level;
			HWIPTV_LOG_INFO("sw_log_set_level %d ---> %d\n",sw_log_get_level(),log_level);
			sw_log_set_level(log_level);
		}
	}
	tmp_time = sw_thrd_get_tick()/1000 - m_commit_time;
	//�����־�ļ��Ƿ�ﵽ����ļ���׼,error0.log
	collect_size = get_collectlog_size(type);
	printf("collect_szie = %d K,tmp_time = %d\n s",collect_size/1024,tmp_time);
	if(collect_size >= m_max_size || tmp_time >= max_time) //�̶��������� 
	{
		char file_name[64] = {0};
		HWIPTV_LOG_DEBUG("collect_szie = %dK,tmp_time = %ds\n",collect_size/1024,tmp_time);
		clean_log_targets(type);
		chang_tmp_log_path(type,file_name,sizeof(file_name));
		set_log_targets(type);
		commit_log_file(type,file_name);
		m_commit_time = sw_thrd_get_tick()/1000;
	}
	return 0;

}

//=======TMS log========================================================================
//tms log level ת����swlog level
static int get_swlog_level(int tmp)
{
	int ret = -1;
	HWIPTV_LOG_DEBUG("tpm = %d\n",tmp);
	switch(tmp)
	{
		case TMS_LOG_DEBUG:
			ret = LOG_LEVEL_ALL;
			break;
		case TMS_LOG_INFO:
			ret = LOG_LEVEL_INFO;
			break;
		case TMS_LOG_ERROR:
			ret = LOG_LEVEL_ERROR;
			break;
		default:
			ret = LOG_LEVEL_OFF;
	}
	return ret;
}


//ֻ���ӿ�,ֵͨ��value����
int hw_newlog_tms_get_para(char *name,char *value,int size)
{
	char tmp_buf[64] = {0};
	if(strncasecmp(name,"LogLevel",strlen("LogLevel"))==0) //��־����
	{
		if(!sw_parameter_get("loglevel",tmp_buf,sizeof(tmp_buf)))
			sw_snprintf(tmp_buf,sizeof(tmp_buf),0,sizeof(tmp_buf),"0");	
		
	}
	else if(strncasecmp(name,"LogServer",strlen("LogServer"))==0)
	{
		sw_parameter_get("logserver",tmp_buf,sizeof(tmp_buf));
	}
	else if(strncasecmp(name,"LogStartTime",strlen("LogStartTime"))==0)
	{
		if(!sw_parameter_get("logstarttime",tmp_buf,sizeof(tmp_buf)))
			sw_snprintf(tmp_buf,sizeof(tmp_buf),0,sizeof(tmp_buf),"0");
		else
			sw_snprintf(tmp_buf,sizeof(tmp_buf),strlen(tmp_buf),sizeof(tmp_buf),"000"); //��ɺ���
	}
	else if(strncasecmp(name,"LogEndTime",strlen("LogEndTime"))==0) //��־�����ռ���ʱ��
	{
		if(!sw_parameter_get("logstarttime",tmp_buf,sizeof(tmp_buf)))
			sw_snprintf(tmp_buf,sizeof(tmp_buf),0,sizeof(tmp_buf),"0");
		else
			sw_snprintf(tmp_buf,sizeof(tmp_buf),strlen(tmp_buf),sizeof(tmp_buf),"000"); //��ɺ���
	}
	else
	{
		HWIPTV_LOG_DEBUG("unknow para\n");
		return -1;
	}
	sw_snprintf(value,size,0,size,"%s",tmp_buf);
	return 0;
}

//��������
int hw_newlog_tms_set_para(char *name,char *value,int size)
{
	char tmp_buf[64] = {0};
	HWIPTV_LOG_DEBUG("name = %s,value = %s,size = %d\n",name,value,size);

	if(name == NULL || value == NULL || value[0] == '\0' ||size > sizeof(tmp_buf))
	{
		HWIPTV_LOG_DEBUG("para error name = %s,value = %s,size = %d\n",name,value,size);
		return -1;
	}

	unsigned int tmp = 0;
	sw_memcpy(tmp_buf,sizeof(tmp_buf),value,size,size);
	if(strncasecmp(name,"LogLevel",strlen("LogLevel"))==0) //��־����
	{
		tmp = atoi(tmp_buf);
		m_log_level = get_swlog_level(tmp);
		sw_parameter_set("loglevel",tmp_buf); //����TMS��ֵ
		//���й����·���־����ı��
		if(m_log_mode == LOG_MODE_DEBUG && m_log_level < LOG_LEVEL_OFF) 
			set_loglevel(DEBUG_LOG);

	}
	else if(strncasecmp(name,"LogServer",strlen("LogServer"))==0) //��־�ϴ�������
	{
		sw_parameter_set("logserver",tmp_buf);
	}
	else if(strncasecmp(name,"LogStartTime",strlen("LogStartTime"))==0) //��־��ʼ�Ѽ���ʱ��
	{
		tmp = strlen(tmp_buf);
		//�������·���ms,ʹ��ʱת������(��ȥ�����3λ)
		tmp_buf[tmp-3] = '\0';
		tmp = atoi(tmp_buf);
		sw_parameter_set_int("logstarttime", tmp);
		m_start_time = tmp;
	}
	else if(strncasecmp(name,"LogEndTime",strlen("LogEndTime"))==0) //��־�����ռ���ʱ��
	{
		tmp = strlen(tmp_buf);
		//�������·���ms,ʹ��ʱת������(��ȥ�����3λ)
		tmp_buf[tmp-3] = '\0';
		tmp = atoi(tmp_buf);
		sw_parameter_set_int("logendtime", tmp);
		m_end_time = tmp;
	}
	else
	{
		HWIPTV_LOG_DEBUG("unknown para %s\n",name);
		return -1;
	}
	sw_parameter_save();
	return 0;
}

//==========================function=======================
static bool collect_log_proc(uint32_t wparam,uint32_t lparam)
{
	time_t now = 0;
	int count = 0;
	bool time_sync = false; //�Ƿ���ͬ��ʱ��
	char file_name[64] = {0};
	while(!m_proc_exit)
	{
		if(!time_sync) //����Ƿ���ͬ��ʱ��
		{
			if(hw_time_get_state() || sw_get_dvb_sync_time_state())
				time_sync = true;
			else
				HWIPTV_LOG_DEBUG("sys time is not sync\n");
		}

		now = time(NULL);
		if(m_usb_is_collect) //���ȼ����
		{
			int type = 0;
			if(m_log_mode != LOG_MODE_USB )
			{
				if(m_log_mode == LOG_MODE_ERROR)
					type = ERROR_LOG;
				else if(m_log_mode == LOG_MODE_DEBUG)
					type = DEBUG_LOG;
				if(type)
				{
					sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));
					clean_log_targets(type);
					chang_tmp_log_path(type,file_name,sizeof(file_name));
					commit_log_file(type,file_name); //��flash��������־�ϴ�
				}
				m_log_mode = LOG_MODE_USB;
			}
		}
		//TMS off,|| time_sync = false || m_start_time < now || now > m_end_time
		else if(m_log_level < LOG_LEVEL_OFF && time_sync && m_start_time < now && now < m_end_time) //TMS_ץ��־����
		{
			//�����Ѽ���־
			printf("DEBUG =========\n");
			if(m_log_mode != LOG_MODE_DEBUG)
			{
				sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));
				count = 0;
				if(m_log_mode == LOG_MODE_ERROR)
				{
					clean_log_targets(ERROR_LOG);
					if(get_collectlog_size(ERROR_LOG) > 0)
					{
						printf("DEBUG  chagc=========\n");
						chang_tmp_log_path(ERROR_LOG,file_name,sizeof(file_name));
					}
				}
				set_log_targets(DEBUG_LOG);
				set_loglevel(DEBUG_LOG);
				//�����ģʽ�л�,����Ҫ�����ռ�����־commit
				if(m_log_mode == LOG_MODE_ERROR)
				{
					printf("commit file %s\n",file_name);
					commit_log_file(ERROR_LOG,file_name); //����err��־
					sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));
					commit_log_file(DEBUG_LOG,file_name); //��flash��������־�ϴ�
					m_commit_time = sw_thrd_get_tick()/1000;
				}
				m_log_mode = LOG_MODE_DEBUG;
			}
			if(count == 0 || count >= 60) //1�ֻ�ȡһ��
			{
				count = 0;
				printf(" ======get top===\n");
				get_top_info();
				get_proc_info();
			}
			collect_log_check(DEBUG_LOG);
		}
		else //ERROR��־
		{
			//������־�Ѽ�
			printf("ERROR =========\n");
			if(m_log_mode != LOG_MODE_ERROR)
			{
				count = 0;
				sw_memset(file_name,sizeof(file_name),0,sizeof(file_name));	
				if(m_log_mode == LOG_MODE_DEBUG)
				{
					clean_log_targets(DEBUG_LOG);
					if(get_collectlog_size(DEBUG_LOG) > 0)
					{
						printf("ERROR =chang========\n");
						chang_tmp_log_path(DEBUG_LOG,file_name,sizeof(file_name));
					}
				}
				set_log_targets(ERROR_LOG);
				set_loglevel(ERROR_LOG);
				//�����ģʽ�л�,����Ҫ�����ռ�����־commit //������ϴ���־
				if(m_log_mode == LOG_MODE_DEBUG)
				{
					commit_log_file(DEBUG_LOG,file_name);
					m_commit_time = sw_thrd_get_tick()/1000;
				}
				m_log_mode = LOG_MODE_ERROR;
			}
			if(count == 0 || count >= 60) //1�ֻ�ȡһ��
			{
				count = 0;
				get_top_info();
			}
			collect_log_check(ERROR_LOG);	
		}
		count ++;
		sw_thrd_delay(1000);
	}
	m_colloectlog_thrd = NULL;
	clean_log_targets(DEBUG_LOG);
	return false;
}

//��ʼץ��־
void hw_new_log_init(void)
{
	//��������Ŀ¼
	struct stat path;
	char buf[64] = {0};
	int i = 0;
	int j = 0;
	//��ʼ��Ŀ¼
	if(lstat(TMP_LOG_BASE_PATH,&path) != 0)
		mkdir(TMP_LOG_BASE_PATH,600);
	if(lstat(STB_LOG_PATH,&path) != 0)
		mkdir(STB_LOG_PATH,600);
	//��ȡ���ӵ�mac��ַ
	if(!sw_parameter_get("mac",buf,sizeof(buf)))
	{
		sw_snprintf(buf,sizeof(buf),0,sizeof(buf),"FFFFFFFFFFFF");
		HWIPTV_LOG_DEBUG("can't get mac\n");
	}
	else
	{
		sw_memset(m_mac,sizeof(m_mac),0,sizeof(m_mac));
		for(i=0; i< strlen(buf);i++)
		{
			if(buf[i] == ':')
				continue;
			m_mac[j]=buf[i];
			j++;
		}	
	}
	//��ʼ���ɱ���־��Ϣ
	m_chang_info.time_out = 0;
	m_chang_info.level = LOG_LEVEL_OFF;
	//��ʼ��debug��־����
	sw_memset(buf,sizeof(buf),0,sizeof(buf));
	if(sw_parameter_get("loglevel",buf,sizeof(buf)))
		m_log_level = get_swlog_level(atoi(buf));
	else
		 HWIPTV_LOG_WARN("fail to get the para: loglevel\n");

	sw_memset(buf,sizeof(buf),0,sizeof(buf));
	if(sw_parameter_get("logstarttime",buf,sizeof(buf)))
		m_start_time = atoi(buf);
	else
		 HWIPTV_LOG_WARN("fail to get the para: logstarttime\n");

	sw_memset(buf,sizeof(buf),0,sizeof(buf));
	if(sw_parameter_get("logendtime",buf,sizeof(buf)))
		m_end_time = atoi(buf);
	else
		HWIPTV_LOG_WARN("fail to get the para: logendtime\n");

	sw_memset(m_stb_type,sizeof(m_stb_type),0,sizeof(m_stb_type));
	if(!sw_parameter_get("hardware_type",m_stb_type,sizeof(m_stb_type)))
	{
		sw_snprintf(m_stb_type,sizeof(m_stb_type),0,sizeof(m_stb_type),"EC2108CV5");
		HWIPTV_LOG_WARN("fail to get the para: hardware_type\n");
	}

	//��ʼ��error��־����ʱ��
	sw_memset(buf,sizeof(buf),0,sizeof(buf));
	if(sw_parameter_get("TimeIntervalForLogToFlash",buf,sizeof(buf)))
	{
		m_interval_error = atoi(buf);
		if(m_interval_error < 10*60) //��ֹ��������ʱ�����
			m_interval_error = 10*60;
	}
	else
		HWIPTV_LOG_WARN("fail to get the para:TimeIntervalForLogToFlash\n");

	//��������߳�
	m_colloectlog_thrd = sw_thrd_open( "tCollectlog_thrd", 168, 0, 1024*8,collect_log_proc ,0,0);
	if(m_colloectlog_thrd == NULL)
	{
		HWIPTV_LOG_ERROR("fail to open tErrorlog_thrd\n");
		return;
	}
	sw_thrd_resume(m_colloectlog_thrd);
	//ɾ���ϸ��汾����־,��ֹռ�ÿռ�
	return;
}
//ѹ����Ϣ,ֹͣ�Ѽ���־
void hw_new_log_exit(void)
{
	m_proc_exit = true;
	return;
}
int hw_usb_collectlog(bool usb)
{
	int count = 0;
	m_usb_is_collect = usb;
	if(usb)
	{
		for(;;)
		{
			if(m_log_mode == LOG_MODE_USB)
				break;
			if(count > 25)
				break;
			count ++;
			sw_thrd_delay(300);
		}
	}
	else if(m_log_mode == LOG_MODE_USB)
		m_log_mode = LOG_MODE_NULL;
	return 0;
}
