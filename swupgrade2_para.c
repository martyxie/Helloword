/*file:swupgrade_para.c
 *briefs:灭顶之灾 UPG参数升级
 *       将congfig中的信息统一保存到该文件,各局点获取该信息后可进行相应的定制.
 *date:20180502
 *history:noe
 *设计思想:借助原来的升级模块,获取config文件中的[paramreset]标签信息
 *         在应用层,不同的局点获取到信息后可以进行相应的定制
 */
#include "swapi.h"
#include "swupgrade2_priv.h"
#include "swupgrade2_para.h"
//=============
#include "swparameter.h"
#include "swfile.h"
#include "swssl.h"
#include "swjson.h"

#define  UPGRADE_PARA_PATH "/var/para_data"
extern int swsyscmd(const char* cmd );

static swupg_para_t m_upg;

void sw_upgpara_info_reset(void)
{
	sw_memset(&m_upg,sizeof(m_upg), 0, sizeof(m_upg));
	m_upg.is_reboot = 1; //默认强制升级后重启
	return;
}
//设置相关信息到
void upgpara_info_set( swupg_para_t *file,int size)
{
	if(file==NULL || size > sizeof(m_upg))
	{
		sw_log_error("fail to set para info\n");
		return;
	}
	sw_memcpy(&m_upg,sizeof(m_upg),file,size,size);
	return;	
}

//设置服务文件的获取路径
void sw_upgpara_set_readdir(char *dir,int size)
{
	if(dir == NULL || size > sizeof(m_upg.filepath) || m_upg.version[0]=='\0')
	{
		sw_log_error("fail to set file dir\n");
		return;
	}
	sw_memset(m_upg.filepath,sizeof(m_upg.filepath),0,sizeof(m_upg.filepath));
	sw_snprintf(m_upg.filepath,sizeof(m_upg.filepath),0,sizeof(m_upg.filepath),"%s%s",dir,m_upg.filename);
	return;
}
//获取信息
bool sw_upgpara_get_para_info(swupg_para_t *file,int size)
{
	if(file == NULL || size < sizeof(swupg_para_t))
		return false;
	sw_memcpy(file,size,&m_upg,sizeof(m_upg),sizeof(m_upg));
	return true;
}

//================test====

static bool date_is_right(const char *buf)
{
	if(buf == NULL)
		return false;
	int i = 0;
	int len = strlen(buf);
	char tmp[12] = {0};
	if(len != 8+3) //规格规定为 8+3
		return  false;
	sw_memcpy(tmp,sizeof(tmp),buf,len,len);
	for(i=0;i<len;i++) //先检测是否为纯数字
	{
		if(buf[i]< '0' || buf[i] >'9' )
			return false;
	}
	//对日期进行进一步校验,前8字节
	tmp[8] = '\0';
	if(atoi(&tmp[6]) > 31)
		return false;
	else
	{
		tmp[6] = '\0';
		if(atoi(&tmp[4]) > 12)
			return false;
	}
	return true;
}

static bool check_valiable(const char *buf)
{
	//length == 8+3+局点信息
	char hard_type[18] = {0};
	const char *p = NULL;
	if(!sw_parameter_get("hardware_type",hard_type,sizeof(hard_type)))
	{
		printf("fail to get hardware_type\n");
		strlcpy(hard_type,"EC2108CV5",sizeof(hard_type));
	}
	int len = strlen(hard_type)+8+3;
	if(strlen(buf) != len) //检测长度是否正确
	{
		printf("version is not right\n");
		return false;
	}
	//检测版本信息是否正确
	if(strncmp(buf,hard_type,strlen(hard_type)) != 0)
	{
		printf("hardware_type is not match\n");
		return false;
	}
	//检测版本号是否正确
	p=buf+strlen(hard_type);
	if(!date_is_right(p))
	{
		printf("data is not valiable\n");
		return false;
	}
	return true;
}

static bool check_para_config_setting(swupg_para_t *file,int size)
{
	swupg_para_t upg_para;
	if(file == NULL || size > sizeof(upg_para))
		return false;
	sw_memset(&upg_para,sizeof(upg_para),0,sizeof(upg_para));
	sw_memcpy(&upg_para,sizeof(upg_para),file,size,size);
	char *p = NULL;
	int len = 0;
	char last_verison[32] = {0};
	//3个关键信息必须都同时存在
	if(upg_para.version[0] == '\0' || upg_para.filename[0] == '\0' || upg_para.checkcode[0] == '\0')
	{
		printf("version or filename or checkcode is not exist\n");
		return false;
	}
	//检测有有效性
	if(check_valiable(upg_para.version))
	{
		//配置文件的名字
		p = strstr(upg_para.filename,".");
		if(p)
			*p = '\0';
		len = strlen(upg_para.filename);
		if(len == strlen(upg_para.version))
		{
			if(strncmp(upg_para.version,upg_para.filename,len) != 0)
			{
				printf("para filename and version is not macth\n");
				return false;
			}
		}
		else
			return false;
	}
	else
	{
		printf("version number no match\n");
		return false;
	}
	//检测版本号是否更新
	sw_parameter_get("last_para_update_ver",last_verison,sizeof(last_verison));
	if(last_verison[0] != '0')
	{
		if(strncmp(upg_para.version,last_verison,len) <= 0)
		{
			printf("last_para_update_ver is %s\n",last_verison);
			return false;
		}
	}
	return true;
}

typedef struct _para_t
{
	char name[128];
	char value[256];
}para_t;

//升级参数,只升级存在的参数不存在的不升级,权限控制
static int upgrade_para_data(const char *buf,int size)
{
	if(buf == NULL || buf[0] == '\0')
		return -1;
	para_t para_upg;
	char tmp_value[256] = {0};
	int strsize = strlen(buf);
	if(strsize > size)
		strsize = size;
	const char *p, *e, *name_s, *name_e;
	int len = 0;
	int num = 0;
	//分析数据
	printf("buf = %s\n",buf);
	p = buf;
	while(p < buf + size)
	{
		//去掉开始的空格
		while( *p == ' ' ||  *p == '\t' ||  *p == '\r' ||  *p == '\n' )
			p++;
		name_s = p;
		//找到一行的结束
		while( *p != '\r' && *p != '\n' && *p != '\0' )
			p++;
		e = p;

		/* 忽略: 注释,空行,老版本参数信息*/ 
		if( *name_s == '#' || e <= name_s)
			goto NEXT_ROW;
		/* 如果得到一个 标签的开头,则说明已经结束,定制处理*/
		if(*name_s == '[')
		{
			printf("find the end of ParameterResetList\n");
			break;
		}
		/* 找到name/value分隔符，得到参数名称 */
		p = name_s;
		while( *p != ' ' && *p != '\t' && *p != ':' && *p != '=' && p < e )
			p++;
		if( p <= name_s )
			goto NEXT_ROW;
		name_e = p;
		
		//找到 value
		p++;
		while( *p == ' ' || *p == '\t' || *p == ':' || *p == '=' )
			p++;
		if(p <= e)
		{
			//初始化缓存
			sw_memset(&para_upg,sizeof(para_upg),0,sizeof(para_upg));
			sw_memset(tmp_value,sizeof(tmp_value),0,sizeof(tmp_value));
			//将name 保存起来
			len = name_e - name_s;
			sw_memcpy(para_upg.name,sizeof(para_upg.name),name_s,len,len);
			//将值保存起来
			len = e - p;
			sw_memcpy(para_upg.value,sizeof(para_upg.value),p,len,len);
			
			if(para_upg.name[0] == '\0' || para_upg.value[0] == '\0')
				goto NEXT_ROW;

			if(sw_parameter_get(para_upg.name,tmp_value,sizeof(tmp_value))) //确保每次只升级已存在的参数
			{
				if(strstr(para_upg.name,"password")) //如果是更新密码参数
					sw_parameter_safe_set(para_upg.name, para_upg.value);
				else
					sw_parameter_set(para_upg.name,para_upg.value);
				num ++;
			}
		}
NEXT_ROW:
		p = e + 1;
	}
	if(num > 0)
		sw_parameter_save();

	return num;
}

//把配置文件保存待flash中,同时删除旧的文件
static void upgade_para_file_save(const char *filename,const char *data,int size)
{
	struct stat path;
	char buf[48] = {0};
	FILE *fp = NULL;
	int letfsize = size;
	int ret = 0;
	if(lstat(UPGRADE_PARA_PATH,&path) != 0)
		mkdir(UPGRADE_PARA_PATH,0600);
	else //清空该目录下的所有文件
	{
		sw_snprintf(buf,sizeof(buf),0,sizeof(buf)-1,"rm %s/* -f",UPGRADE_PARA_PATH);
		swsyscmd(buf);
	}
	//保存文件
	sw_memset(buf,sizeof(buf),0,sizeof(buf));
	sw_snprintf(buf,sizeof(buf),0,sizeof(buf)-1,"%s/%s",UPGRADE_PARA_PATH,filename);
	printf("filename = %s\n",buf);
	fp = fopen(buf,"w");
	if(fp == NULL)
	{
		printf("fail to open %s\n",buf);
		return;
	}
	// 写数据
	do{
		ret = fwrite(data,sizeof(char),letfsize,fp);
		letfsize -= ret;
	}while(letfsize > 0 && ret > 0);
	fclose(fp);
	fp = NULL;
	return;
}

//sha256的对比,一致返回true
static bool sha256_check(const char *data,int size,const char *sha256_code)
{
	if(data == NULL || sha256_code == NULL )
		return  false;

	unsigned char chip_text[32] = {0};
	char checkcode[68] = {0};// 计算sha256的值
	int i = 0;
	int pos = 0;
	int ret = 0;
	//对比sha256的值
	ret = sw_sha256_sum(chip_text, (unsigned char *)data,size);
	for(i=0;i<ret;i++)
		pos += snprintf(&checkcode[pos],sizeof(checkcode)-pos,"%02x",chip_text[i]);
	if(strncmp(checkcode,sha256_code,pos) != 0)
	{
		printf("checkcode is no macth\n");
		return false;
	}
	return true;
}

typedef struct _para_fileter
{
	char *name;
	bool result;
}para_fileter_t;

static bool para_check_softversion(const char *list,const char *key,int keylen)
{
	if(list == NULL || key == NULL)
		return false;
	if(strncmp(list,key,keylen) != 0)
	{
		printf("list and  key is not macth\n");
		return false;
	}
	const char *p,*e,*value_s;
	char huawei_version[48] = {0};
	int len = 0;
	p = list;
	e = p+strlen(p);
	value_s = NULL;
	while( *p != '\t' && *p != '\r' && *p != '\n' && *p != '\0' && p < e )
	{
		if(*p == '=')
		{
			value_s = ++p;
			break;
		}
		p++;
	}
	if(value_s == NULL) //说明没有指定版本
		return true;
	printf(" p ==%p\n",p);
	while( *p == ' ' && *p != '\0' && p < e) //去掉开始的空格
		p++;
	value_s = p;

	printf(" p ==%p\n",p);
	while(*p != '\t' && *p != '\r' && *p != '\n' && *p != '\0' && p < e ) //找到结束
		p++;
	//将末尾的空格去掉
	printf(" p ==%p\n",p);
	do{
		p--;
	}while(*p == ' ' && p > value_s);

	len = p - value_s + 1; //收尾字符要算上
	if(len == 0)
		return  true;
	else if(len != len)
	{
		printf("len can not macth\n");
		return false;
	}
	//对比版本号
	if(strncasecmp(value_s,"EC2108CV5 V100R001C00LMXM37SPC001B001",len) == 0)
		return true;
	else
	{
		printf("version is not macth ,%s\n",HUAWEI_VERSION);
		return false;
	}	
}

static bool check_ip(const char *ip)
{
	if(ip == NULL)
		return false;
	int len = strlen(ip);
	if(len > 15)
		return false;
	char tmp_buf[16] = {0};
	sw_memcpy(tmp_buf,sizeof(tmp_buf),ip,len,len);
	int i = 0;
	int count = 0;
	int num = 0;
	char *p = tmp_buf;

	for(i = 0; i < len; i++)
	{
		if(tmp_buf[i] == '.')
		{
			count ++;
			tmp_buf[i]='\0';
			num = atoi(p);
			if(num > 255)
				return false;
			p = &tmp_buf[i+1];
		}else if(tmp_buf[i] == ' ')
		{
			printf("ip string include space char\n");
			return  false;
		}
	}
	if(count == 3)
		return true;

	return false;
}

//json 中的内容解析
static bool para_fileter_priv_info(const char *formname,const char *formvalue,const char *toname,const char *tovalue)
{
	if(formname == NULL || formvalue == NULL || toname == NULL || tovalue == NULL)
		return false;
	//分别比较
	char tmp_buf[32] = {0};
	int form_len = strlen(formvalue);
	int to_len = strlen(tovalue);

	if(strncasecmp(formname,"FromMac",strlen("FromMac")) == 0 && strncasecmp(toname,"toMac",strlen("toMac")) == 0)
	{
		sw_strlcpy(tmp_buf,sizeof(tmp_buf),sw_network_get_mac(),sizeof(tmp_buf) -1);
		int mac_len = strlen(tmp_buf);
		printf("mac is %s\n",tmp_buf);

		if(form_len != to_len || mac_len != to_len)
		{
			printf("mac len is not macth\n");
			return false;
		}
		if(strncasecmp(formvalue,tmp_buf,mac_len) <= 0 && strncasecmp(tmp_buf,tovalue,mac_len) <= 0)
		{
			printf("this mac will be allowed\n");
			return  true;
		}
	}
	else if(strncasecmp(formname,"FromUser",strlen("FromUser")) == 0 && strncasecmp(toname,"ToUser",strlen("ToUser")) == 0)
	{
		sw_parameter_get("ntvuseraccount",tmp_buf,sizeof(tmp_buf));
		int usr_len = strlen(tmp_buf);
		int i = 0;
		printf("ntvuseraccount is %s\n",tmp_buf);
		if(tmp_buf[0] == '\0')
		{
			printf("not usr_account to cmp\n");
			return  false;
		}
		if( form_len != to_len) //规格要求,不一样的情况下,均为数字,并且最大不能超过20个
		{
			if(form_len > 20 || to_len > 20)
			{
				printf("fileist usr acount len too long\n");
				return false;
			}

			for(i = 0; i < usr_len; i++)
			{
				if( tmp_buf[i] < '0' || tmp_buf[i] > '9')
				{
					printf("usr acount is not all digital\n");
					return false;
				}
			}

			for(i = 0; i < form_len; i++)
			{
				if( formvalue[i] < '0' || formvalue[i] > '9')
				{
					printf("filelist form acount is not all digital\n");
					return false;
				}
			}

			for(i = 0; i < to_len; i++)
			{
				if( tovalue[i] < '0' || tovalue[i] > '9')
				{
					printf("filelist to acount is not all digital\n");
					return false;
				}
			}
			//纯数字的情况下先比较长度,长度短的小
			if(form_len < to_len && usr_len <= to_len)
			{
				if(form_len == usr_len)
				{
					if(strncmp(formvalue,tmp_buf,usr_len) <= 0)
						return true;
				}
				else if(to_len == usr_len)
				{
					if(strncmp(tmp_buf,tovalue,usr_len) <= 0)
						return true;
				}
				else
					return true;
			}
		}
		else if(form_len == usr_len) //与盒子的usrid长度一样,才认为头端配置正确
		{
			if(strncmp(formvalue,tmp_buf,usr_len)<=0 && strncmp(tmp_buf,tovalue,usr_len)<=0)
				return true;
		}
	}
	else if(strncasecmp(formname,"FromIp",strlen("FromIp")) == 0 && strncasecmp(toname,"ToIp",strlen("ToIp")) == 0)
	{
		sw_strlcpy(tmp_buf,sizeof(tmp_buf),sw_network_get_currentip(),sizeof(tmp_buf) -1);

		in_addr_t local_ip = 0;
		in_addr_t form_ip = 0;
		in_addr_t to_ip = 0;

		local_ip = inet_addr(tmp_buf);

		if(check_ip(formvalue) && check_ip(tovalue)) //校验合法性
		{
			form_ip = inet_addr(formvalue);
			to_ip = inet_addr(tovalue);

			if(form_ip <= local_ip && local_ip <= to_ip)
			{
				printf("this ip will be allow\n");
				return true;
			}
		}
		printf("loacl_ip not included ,loca_ip = %u,form_ip = %u,to_ip = %u\n",local_ip,form_ip,to_ip);
	}
	printf("not filelist is not match\n");
	return  false;
}

//传进来的 list 的开始就是key
static bool para_fileter_decode(const char *list,const char *key,int keylen)
{
	if(list == NULL || key == NULL)
		return false;
	if(strncmp(list,key,keylen) != 0)
	{
		printf("list and  key is not macth\n");
		return false;
	}
	const char *list_s, *list_e;
	sw_json_t *json = NULL;
	sw_json_t *tmp_json = NULL;
	char json_data[1024] = {0};
	int len = 0;
	int i = 0,k = 0;
	bool result = false;

	list_s = strstr(list,"[");
	if(list_s == NULL)
	{
		printf("json list not true\n");
		return false;
	}
	list_e = strstr(list_s,"]");
	if(list_e == NULL)
	{
		printf("json list not true\n");
		return false;
	}
	len = list_e - list_s + 1; //得到数据的大小
	if(len > sizeof(json_data))
	{
		printf("fileter list is to long\n");
		return false;
	}
	//sw_memcpy(json_data,sizeof(json_data),list_s,len,len);
	json = sw_json_decode(list_s,len,json_data,sizeof(json_data));
	if(json == NULL || json->type != VTYPE_ARRAY)
	{
		printf("somthing wrong in json_decode\n");
		return false;
	}
	//比较解析到的信息
	for(i=0; i<(json->jarr->count) && i<5; i++) //obj ,最多比较5个
	{
		tmp_json = &(json->jarr->values[i]); //每个obj 的地址
		if(tmp_json == NULL || tmp_json->jobj->count != 2)
		{
			printf("obj is NULL or count not eq 2\n");
			return false;
		}
		for(k = 0; k < tmp_json->jobj->count; k++) //实现比较
			printf("name[%d] = %s,value[%d]= %s\n",k,tmp_json->jobj->nvs[k].name,k,tmp_json->jobj->nvs[k].value.str);
		//只要有一项符合即可
		if(para_fileter_priv_info(tmp_json->jobj->nvs[0].name,tmp_json->jobj->nvs[0].value.str,tmp_json->jobj->nvs[1].name,tmp_json->jobj->nvs[1].value.str))
		{
			result = true;
			break;
		}
	}

	return true;
}
//条件检测
static bool upg_para_fileter(const char *list,int size)
{
	if(list == NULL)
		return false;
	char *p = NULL;
	para_fileter_t fileter[4] = {{"Version",true},{"UserRange",true},{"IpRange",true},{"MacRang",true}};
	int len = 0;
	int i = 0;
	//sw_json_decode();
	for(i=0; i<4; i++)
	{
		p = strstr(list,fileter[i].name);
		if(p)
		{
			len = strlen(fileter[i].name);
			if(i==0)
				fileter[i].result = para_check_softversion(p,fileter[i].name,len); //version 不是json格式,因此单独处理
			else
				fileter[i].result = para_fileter_decode(p,fileter[i].name,len);
			if(!fileter[i].result)
				break;
		}
	}
	return (fileter[0].result && fileter[1].result && fileter[2].result && fileter[3].result);
}
//do updata
static bool upgrade_para(const swupg_para_t *upg)
{
	if(upg == NULL || upg->filepath[0] == '\0')
	{
		printf("no file path\n");
		return false;
	}
	int timeout = 15000;
	int file_size = 0;
	char *data_buf = NULL;
	int read_size = 0; //读取的大小
	int recv_size = 0; //已读的大小
	int ret = 0; //实际读到的大小
	int left_size = 0;  //剩余要读的大小
	bool result = false;
	bool need_upg = true;
	char *p = NULL;
	char *tmp = NULL;

	swfile_t *fp = sw_file_open_ex(upg->filepath, "r", timeout, "connecttimes=2000,4000,8000\0");//华为升级规格需要
	if(fp == NULL)
	{
		printf("can't to find the  file\n");
		return false;
	}
	file_size = sw_file_get_size(fp);
	if(file_size <= 0)
	{
		printf("not content \n");
		goto ERROR;
	}
	data_buf = (char *)malloc(file_size+8);//预留8字节
	if(data_buf == NULL)
	{
		printf("fail to malloc mem\n");
		goto ERROR;
	}
	sw_memset(data_buf,file_size+8,0,file_size+8);
	
	left_size = file_size; 
	//下载数据
	do{
		recv_size += ret;
		left_size -= ret;
		read_size = left_size > 160*1024 ? 160*1024:left_size;
	}while((ret = sw_file_read( fp,data_buf+recv_size,read_size)) > 0);
	if(recv_size != file_size)
	{
		printf("fail to recv data\n");
		goto ERROR;
	}
	if(sha256_check(data_buf,file_size,upg->checkcode) == false)
		;//goto ERROR;

	//找到参数的空间,以及找到过滤项空间
	p = strstr( data_buf,"[TMWParameterResetList]");
	tmp = strstr( data_buf,"[STBFliterList]");
	if(tmp && p) //查下是否符合升级条件
		need_upg = upg_para_fileter(tmp,strlen(tmp));
	else if(p)
		need_upg = true;
	else
		need_upg = false;

	if(need_upg)
	{
		printf("begin to upgrade para\n");
		//将下载到的文件保存到flase中
		upgade_para_file_save(upg->filename,data_buf,file_size);
		p += strlen("[TMWParameterResetList]");
		//支持 [TMWParameterResetList],与[STBFliterList] 不是相邻的情况
		ret = upgrade_para_data(p,strlen(p));
		if(ret > 0)
		{
			sw_parameter_set("last_para_update_ver",(char *)upg->version);
			sw_parameter_save();
			printf("%d param had finish reset\n", ret);
		}
		else
			printf("ret is %d\n",ret);
	}

	result = need_upg;
ERROR:
	if(fp)
		sw_file_close(fp);
	if(data_buf)
		free(data_buf);
	return result;
}

void sw_upgpara_begin(void)
{
	if(!check_para_config_setting(&m_upg,sizeof(m_upg)))
		printf("not any para data to do\n");
	else
	{
		if(!upgrade_para(&m_upg))
			printf("update para faile or not allow to upg para\n");
		else
		{
			if(m_upg.is_reboot == 1)
			{
				sleep(3);
				printf("reboot\n");
			}
		
		}
	}
	return;
}
