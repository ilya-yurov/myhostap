/*******************************************************
 * Минимальная программная реализация точки доступа.   *
 * Реализована возможность использования исключительно *
 * с параметром "WLAN_AUTH_OPEN" (открытая сеть).      *
 * Приложение  использует интерфейс nl80211            *
 * для связи с cfg80211                                *
 *******************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>

#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>
#include <netlink/msg.h>
#include <netlink/attr.h>
#include <errno.h>
#include <linux/nl80211.h>

#include "myhostap.h"

//Основные данные точки доступа
u8 ether[ETH_ALEN]={0xD0, 0x0E, 0xA4, 0x81, 0xfc, 0x00}; //MAC-адрес интерфейса wlan0 
u16 beacon_int = 200;
char ssid[]="TEST_OPEN_WIRELESS_NETWORK";                //определяем SSID сети
u8 channel = 8;                                          //определяем канал сети
struct nl80211_state nlstate;
signed long long devidx = 0;
struct rates used_rates[8];
	

/*Управление точкой доступа*/
#define MAX_STATION_COUNT 20                              //определяем максимальное количество подключенных клиентов
struct station_data stations[MAX_STATION_COUNT];
int station_cnt;

#define AID_WORDS ((2008 + 31) / 32)
u32 sta_aid[AID_WORDS];


static int error_handler(struct sockaddr_nl *nla, struct nlmsgerr *err,
			 void *arg)
{
	int *ret = arg;
	*ret = err->error;
	printf("->CFG80211 returns: error: No:%d, %s\n",err->error,
		strerror((-1)*err->error));
	return NL_STOP;
}


static int error_handler_test(struct sockaddr_nl *nla, struct nlmsgerr *err,
			 void *arg)
{
	int *ret = arg;
	*ret = err->error;
	printf("!!!ERROR TEST HANDLER called: err->error:%d\n",err->error);
	return NL_STOP;
}


static int finish_handler(struct nl_msg *msg, void *arg)  //функция finish_handler позволит нам получать сообщения от ядра
{
	int *ret = arg;
	*ret = 0;
	printf("Finish handerl called\n");
	return NL_SKIP;
}

static int ack_handler(struct nl_msg *msg, void *arg)
{
	int *ret = arg;
	*ret = 0;
	printf("->CFG80211 returns: Request acknowledged\n");
	return NL_STOP;
}

static int mesgmal_handler(struct nl_msg *msg, void *arg)
{
	printf("-> CFG80211 returns: malformed message received\n");
	return NL_OK;
}

static int process_mgmt_handler(struct nl_msg *msg, void *arg) // основная функция управления параметрами точки доступа
{
	printf("-----------------------\nMgmt Frame handler called from ");
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));  // 
	struct nlattr *tb[NL80211_ATTR_MAX + 1];
	int ifidx = -1;
	struct nlattr *frame=NULL;
	const u8 *data;
	size_t len;
	const struct ieee80211_mgmt *mgmt;
	int i;
	u16 fc, stype;

	nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);
	if (tb[NL80211_ATTR_IFINDEX])
	{
		ifidx = nla_get_u32(tb[NL80211_ATTR_IFINDEX]);
		printf("interface %d\n",ifidx);
	}
	frame=tb[NL80211_ATTR_FRAME];
	data = nla_data(frame);
	len = nla_len(frame);
	mgmt = (const struct ieee80211_mgmt *) data;
	printf("Source Address:");
	for(i=0;i<6;i++)
	{
		printf("%x",mgmt->sa[i]);
		if(i<5)
			printf(":");
	}
	printf("\n");
	printf("Destination Address:");
	for(i=0;i<6;i++)
	{
		printf("%x",mgmt->da[i]);
		if(i<5)
			printf(":");
	}
	printf("\n");
	printf("BSSID:");
	for(i=0;i<6;i++)
	{
		printf("%x",mgmt->bssid[i]);
		if(i<5)
			printf(":");
	}
	printf("\n");
	
	printf("%s, ",nl80211_command_to_string(gnlh->cmd));
	switch(gnlh->cmd)
	{
		case NL80211_CMD_AUTHENTICATE:
		case NL80211_CMD_ASSOCIATE:
		case NL80211_CMD_DEAUTHENTICATE:
		case NL80211_CMD_DISASSOCIATE:
		case NL80211_CMD_FRAME_TX_STATUS:
		case NL80211_CMD_UNPROT_DEAUTHENTICATE:
		case NL80211_CMD_UNPROT_DISASSOCIATE:
		case NL80211_CMD_PROBE_CLIENT:
		{
			printf("Not a command frame, processing stopped.\n");
			break;
		}
		case NL80211_CMD_FRAME:
		{
			fc = le_to_host16(mgmt->frame_control);
			stype = WLAN_FC_GET_STYPE(fc);
			switch(stype)
			{
				case WLAN_FC_STYPE_AUTH			:
				{
					printf("WLAN_FC_STYPE_AUTH Frame\n");
					u16 auth_alg, auth_transaction, status_code;
					u16 auth_resp = WLAN_STATUS_SUCCESS;
					u8 auth_resp_ies[2 + WLAN_AUTH_CHALLENGE_LEN];
					size_t auth_resp_ies_len = 0;
					
					auth_alg = le_to_host16(mgmt->u.auth.auth_alg);
					auth_transaction = 
						le_to_host16(mgmt->u.auth.auth_transaction);
					status_code = le_to_host16(mgmt->u.auth.status_code);
					printf("auth_alg = %d, auth_trans = %d, status_code= %d\n",auth_alg, auth_transaction, status_code);
					auth_transaction++;
					station_cnt++;
					if(station_cnt<MAX_STATION_COUNT)
					{
						memcpy(stations[station_cnt-1].mac, mgmt->sa, 
							ETH_ALEN);
						stations[station_cnt-1].pre_auth=1;
					}
					else
					{
						printf("Not able to handle a new station");
						printf("MAX_STATION_COUNT reached\n");
						break;
					}
					
					switch (auth_alg) 
					{
						case WLAN_AUTH_OPEN:
						{
							printf("WLAN_AUTH_OPEN\n");
							struct ieee80211_mgmt *auth_reply;
							u8 *auth_rep_buf;
							size_t auth_rlen;
							
							auth_rlen = IEEE80211_HDRLEN + 
								sizeof(auth_reply->u.auth) + 
									auth_resp_ies_len;
							auth_rep_buf = malloc(auth_rlen);
							memset(auth_rep_buf, 0, auth_rlen);
							auth_reply = 
								(struct ieee80211_mgmt *) auth_rep_buf;
							auth_reply->frame_control = 
								IEEE80211_FC(WLAN_FC_TYPE_MGMT,
													WLAN_FC_STYPE_AUTH);
							memcpy(auth_reply->da, mgmt->sa, ETH_ALEN);
							memcpy(auth_reply->sa, ether, ETH_ALEN);
							memcpy(auth_reply->bssid, ether, ETH_ALEN);
							auth_reply->u.auth.auth_alg = 
								host_to_le16(auth_alg);
							auth_reply->u.auth.auth_transaction = 
								host_to_le16(auth_transaction);
							auth_reply->u.auth.status_code = 
								host_to_le16(auth_resp);
							if (auth_resp_ies && auth_resp_ies_len)
								memcpy(auth_reply->u.auth.variable, 
									auth_resp_ies, auth_resp_ies_len);
							
							msg = nlmsg_alloc();
							genlmsg_put(msg, 0, 0, nlstate.nl80211_id,
								0, 0, NL80211_CMD_FRAME, 0);
							NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);
							NLA_PUT(msg, NL80211_ATTR_FRAME, auth_rlen, 
								auth_reply);
							
							struct nl_cb *cb;
							int err = -ENOMEM;
							cb = nl_cb_alloc(NL_CB_DEFAULT);
							err = nl_send_auto_complete(nlstate.nl_sock, 
								msg);
							err = 1;
							nl_cb_err(cb, NL_CB_CUSTOM, error_handler, 
								&err);
							nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, 
								finish_handler, &err);
							nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, 
								ack_handler, &err);
							while (err > 0) {
								int res = nl_recvmsgs(nlstate.nl_sock, cb);		
							}
							printf("Auth Response Sent!\n");
							nl_cb_put(cb);
							nlmsg_free(msg);						
							break;
						}/*Окончание case WLAN_AUTH_OPEN:*/
						case WLAN_AUTH_SHARED_KEY:
						{
							printf("WLAN_AUTH_SHARED_KEY not supported\n");
							break;
						}
						default:
						{
							printf("Auth. Alg. not supported\n");
							break;
						}
					}/* Окончание switch (auth_alg) */
						
					break;
				}/*Окончание case WLAN_FC_STYPE_AUTH	*/
				case WLAN_FC_STYPE_ASSOC_REQ    :
				{
					printf("WLAN_FC_STYPE_ASSOC_REQ Frame\n");
					u16 ass_capab_info, listen_interval;
					u16 ass_resp = WLAN_STATUS_SUCCESS;
					const u8 *ass_pos;
					int left;
					int station_index;
					int ass_send_len;
					u8 *supp_rates_len_marker, *supp_rates_marker;
					u8 ass_buf[sizeof(struct ieee80211_mgmt) + 1024];
					struct ieee80211_mgmt *ass_reply;
					u8 *ass_p;
					
					ass_capab_info = 
						le_to_host16(mgmt->u.assoc_req.capab_info);
					listen_interval = le_to_host16(
						mgmt->u.assoc_req.listen_interval);
					left = len - (IEEE80211_HDRLEN + 
						sizeof(mgmt->u.assoc_req));
					ass_pos = mgmt->u.assoc_req.variable;
					int j;
					for(i=0; i<station_cnt; i++)
					{
						j=memcmp(stations[i].mac, mgmt->sa, sizeof(u8)*ETH_ALEN);
						if(j==0)
						{
							break;
						}	 
					}
					station_index=i;
					if(j!=0||stations[station_index].pre_auth==0)
					{
						printf("Station not authenticated!\n");
						break;
					}	
					stations[station_index].pre_ass=1;					
					memset(ass_buf, 0, sizeof(ass_buf));
					ass_reply = (struct ieee80211_mgmt *) ass_buf;
					ass_reply->frame_control = IEEE80211_FC(WLAN_FC_TYPE_MGMT, 
						WLAN_FC_STYPE_ASSOC_RESP);
					memcpy(ass_reply->da, mgmt->sa, ETH_ALEN);
					memcpy(ass_reply->sa, ether, ETH_ALEN);
					memcpy(ass_reply->bssid, ether, ETH_ALEN);
					ass_send_len = IEEE80211_HDRLEN;
					ass_send_len += sizeof(ass_reply->u.assoc_resp);
					ass_reply->u.assoc_resp.capab_info =
						host_to_le16(WLAN_CAPABILITY_ESS);
					ass_reply->u.assoc_resp.status_code = 
						host_to_le16(ass_resp);
					ass_reply->u.assoc_resp.aid = host_to_le16(
						stations[station_index].aid | BIT(14) | BIT(15));
					ass_p=ass_reply->u.assoc_resp.variable;
					*ass_p++ = WLAN_EID_SUPP_RATES;
					supp_rates_len_marker=ass_p;
					*ass_p++ = 8;
					supp_rates_marker=ass_p;
					for (i=0; i<8; i++) 
					{
						*ass_p = used_rates[i].rate / 5;
						if (used_rates[i].flags & RATE_BASIC)
							*ass_p |= 0x80;
						ass_p++;
					}
					
					ass_send_len += ass_p - ass_reply->u.assoc_resp.variable;
					 
					msg = nlmsg_alloc();
					genlmsg_put(msg, 0, 0, nlstate.nl80211_id,
						0, 0, NL80211_CMD_FRAME, 0);
					NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);
					NLA_PUT(msg, NL80211_ATTR_FRAME, ass_send_len, ass_reply);
					
					struct nl_cb *cb_a;
					int err_a = -ENOMEM;
					cb_a = nl_cb_alloc(NL_CB_DEFAULT);
					err_a = nl_send_auto_complete(nlstate.nl_sock, msg);
					err_a = 1;
					nl_cb_err(cb_a, NL_CB_CUSTOM, error_handler, &err_a);
					nl_cb_set(cb_a, NL_CB_FINISH, NL_CB_CUSTOM, 
						finish_handler, &err_a);
					nl_cb_set(cb_a, NL_CB_ACK, NL_CB_CUSTOM, 
						ack_handler, &err_a);
					
					while (err_a > 0) {
						int res = nl_recvmsgs(nlstate.nl_sock, cb_a);
					}
					printf("Ass Response Sent!\n");
					nl_cb_put(cb_a);
					nlmsg_free(msg);
					
					printf("Trying to add new station in driver...\n");
					struct nl_cb *cb_newsta;
					int err_newsta = -ENOMEM;
					msg = nlmsg_alloc();
					genlmsg_put(msg, 0, 0, nlstate.nl80211_id,
						0, 0, NL80211_CMD_NEW_STATION, 0);
					NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);
					NLA_PUT(msg, NL80211_ATTR_MAC, ETH_ALEN, mgmt->sa);
					NLA_PUT(msg, NL80211_ATTR_STA_SUPPORTED_RATES, 
						*supp_rates_len_marker, supp_rates_marker);
					NLA_PUT_U16(msg, NL80211_ATTR_STA_AID, 1);
					NLA_PUT_U16(msg, NL80211_ATTR_STA_LISTEN_INTERVAL,
						listen_interval);
					cb_newsta = nl_cb_alloc(NL_CB_DEFAULT);
					err_newsta = nl_send_auto_complete(nlstate.nl_sock, 
						msg);
					nl_cb_err(cb_newsta, NL_CB_CUSTOM, 
						error_handler_test, &err_newsta);
					nl_cb_set(cb_newsta, NL_CB_FINISH, NL_CB_CUSTOM, 
						finish_handler, &err_newsta);
					nl_cb_set(cb_newsta, NL_CB_ACK, NL_CB_CUSTOM, 
						ack_handler, &err_newsta);
					
					err_newsta=1;
					
					while (err_newsta > 0) {
						int res = nl_recvmsgs(nlstate.nl_sock, cb_newsta);
						printf("new sta add, err:%d\n",err_newsta);
					}
					nl_cb_put(cb_newsta);
					nlmsg_free(msg);
					
					printf("Trying to set auth flag in driver...\n");
					struct nl_cb *cb_auth;
					int err_auth = -ENOMEM;
					msg = nlmsg_alloc();
					struct nlattr *flags;
					genlmsg_put(msg, 0, 0, nlstate.nl80211_id,
						0, 0, NL80211_CMD_SET_STATION, 0);
					NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);
					NLA_PUT(msg, NL80211_ATTR_MAC, ETH_ALEN, mgmt->sa);
					nla_nest_start(msg, NL80211_ATTR_STA_FLAGS);
					flags = nla_nest_start(msg, NL80211_ATTR_STA_FLAGS);
					if (!flags)
						printf("!flags\n");
					NLA_PUT_FLAG(msg, NL80211_STA_FLAG_AUTHORIZED);
					nla_nest_end(msg, flags);
					
					cb_auth = nl_cb_alloc(NL_CB_DEFAULT);
					err_auth = nl_send_auto_complete(nlstate.nl_sock, msg);
					err_auth = 1;
					nl_cb_err(cb_auth, NL_CB_CUSTOM, error_handler_test, 
						&err_auth);
					nl_cb_set(cb_auth, NL_CB_FINISH, NL_CB_CUSTOM, 
						finish_handler, &err_auth);
					nl_cb_set(cb_auth, NL_CB_ACK, NL_CB_CUSTOM, 
						ack_handler, &err_auth);
						
					err_auth=1;
					while (err_auth > 0) {
						int res = nl_recvmsgs(nlstate.nl_sock, cb_auth);
						printf("auth-flag set, err:%d\n",err_auth);
					}
					nl_cb_put(cb_auth);
					nlmsg_free(msg);

					break;
				}/*Окончание case WLAN_FC_STYPE_ASSOC_REQ */
				case WLAN_FC_STYPE_REASSOC_REQ  :
				{
					printf("WLAN_FC_STYPE_REASSOC_REQ Frame\n");
					break;
				}
				case WLAN_FC_STYPE_DISASSOC     :
				{
					printf("WLAN_FC_STYPE_DISASSOC Frame\n");
					break;
				}
				case WLAN_FC_STYPE_DEAUTH       :
				{
					printf("WLAN_FC_STYPE_DEAUTH Frame\n");
					break;
				}
				case WLAN_FC_STYPE_ACTION       :
				{
					printf("WLAN_FC_STYPE_ACTION Frame\n");
					break;
				}
				case WLAN_FC_STYPE_PROBE_REQ    :
				{
					printf("WLAN_FC_STYPE_PROBE_REQ Frame\n");
					const u8 *ie;
					size_t ie_len;
					size_t resp_len;
					const struct ieee80211_mgmt *req = mgmt;
					ie = mgmt->u.probe_req.variable;
					if (len < IEEE80211_HDRLEN + sizeof(mgmt->u.probe_req))
						return;
					ie_len = len - (IEEE80211_HDRLEN + 
						sizeof(mgmt->u.probe_req));
					
					/*Генерируем Probe Response*/
					printf("Generating Probe Response...\n");
					struct ieee80211_mgmt *resp;
					u8 *pos, *epos;
					size_t buflen;
					#define MAX_PROBERESP_LEN 768
					buflen = MAX_PROBERESP_LEN;
					
					resp = malloc(MAX_PROBERESP_LEN);
					memset(resp, 0, MAX_PROBERESP_LEN);
					epos = ((u8 *) resp) + MAX_PROBERESP_LEN;
					resp->frame_control = IEEE80211_FC(WLAN_FC_TYPE_MGMT,
					   WLAN_FC_STYPE_PROBE_RESP);
					 
					memcpy(resp->da, req->sa, ETH_ALEN);
					memcpy(resp->sa, ether, ETH_ALEN);
					memcpy(resp->bssid, ether, ETH_ALEN);
					resp->u.probe_resp.beacon_int =
							host_to_le16(beacon_int);
					
					int capab = WLAN_CAPABILITY_ESS;
					resp->u.probe_resp.capab_info = host_to_le16(capab);
					
					pos = resp->u.probe_resp.variable;
					*pos++ = WLAN_EID_SSID;
					*pos++ = strlen(ssid);
					memcpy(pos, ssid, strlen(ssid));
					pos += strlen(ssid);
					
					/*Поддерживаемые rates*/
					*pos++ = WLAN_EID_SUPP_RATES;
					*pos++ = 8; //# of rates
					for (i=0; i<8; i++) 
					{
						*pos = used_rates[i].rate / 5;
						if (used_rates[i].flags & RATE_BASIC)
							*pos |= 0x80;
						pos++;
					}
					
					/*DS параметр (текущий канал)*/
					*pos++ = WLAN_EID_DS_PARAMS;
					*pos++ = 1;
					*pos++ = channel;		 
					resp_len = pos - (u8 *) resp;
					int noack=0;
					if(is_broadcast_ether_addr(mgmt->da))
					{
						noack=1;
					}		
					/*Попытка отправить сообщение netlink*/
					struct nl_msg *msg;
					msg = nlmsg_alloc();
					genlmsg_put(msg, 0, 0, nlstate.nl80211_id,
						0, 0, NL80211_CMD_FRAME, 0);	
					NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);
					if (noack)
						NLA_PUT_FLAG(msg, NL80211_ATTR_DONT_WAIT_FOR_ACK);
					NLA_PUT(msg, NL80211_ATTR_FRAME, resp_len, resp);
					struct nl_cb *cb_prob;
					int err_prob = -ENOMEM;
					cb_prob = nl_cb_alloc(NL_CB_DEFAULT);
					err_prob = nl_send_auto_complete(nlstate.nl_sock, msg);
					err_prob = 1;
					nl_cb_err(cb_prob, NL_CB_CUSTOM, error_handler, &err_prob);
					nl_cb_set(cb_prob, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &err_prob);
					nl_cb_set(cb_prob, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &err_prob);

					while (err_prob > 0) {
						int res = nl_recvmsgs(nlstate.nl_sock, cb_prob);
						
					}
					printf("Probe Response Sent!\n");
					nl_cb_put(cb_prob);
					nlmsg_free(msg);
					break;
				}/*Окончание case WLAN_FC_STYPE_PROBE_REQ*/
				default:
				{
					printf("Unknown Frame\n");
					break;
				}
			}/*Окончание switch(stype)*/
			break;
		}/*Окончание case NL80211_CMD_FRAME:*/
			
		default:
		{
			printf(":-( something else...\n");
			break;
		}
		
	}/*Окончание switch(gnlh->cmd)*/
	printf("-----------------------\n");
	return NL_OK;
	
nla_put_failure:
	printf("NLA_PUT_FAILURE\n");
	return -ENOBUFS;
}





static int nl80211_init(struct nl80211_state *state)  // функция инициализирует связь с ядром
{
	int err;

	state->nl_sock = nl_socket_alloc();  // выделяем сокет netlink
	if (!state->nl_sock) {
		fprintf(stderr, "Failed to allocate netlink socket.\n");
		return -ENOMEM;
	}

	nl_socket_set_buffer_size(state->nl_sock, 8192, 8192); //устанавливаем размер буфера сокета

	if (genl_connect(state->nl_sock)) { //при помощи функции genl_connect мы подключаемся к сокету netlink
		fprintf(stderr, "Failed to connect to generic netlink.\n");
		err = -ENOLINK;
		goto out_handle_destroy;
	}

	state->nl80211_id = genl_ctrl_resolve(state->nl_sock, "nl80211"); //просим ядро преобразовать имя nl80211 в id (семейный идентификатор)
	if (state->nl80211_id < 0) {
		fprintf(stderr, "nl80211 not found.\n");
		err = -ENOENT;
		goto out_handle_destroy;
	}

	return 0;

out_handle_destroy:
	nl_socket_free(state->nl_sock);
	return err;
}


int ieee80211_channel_to_frequency(u8 chan, enum nl80211_band band)
{
	/* В стандарте  IEEE 802.11 17.3.8.3.2 и в Приложении (Annex) J,     * 
	 * имеются перекрывающиеся номера каналов в диапазонах 5 ГГц и 2 ГГц */
	if (chan <= 0)
		return 0; /* не поддерживается */
	switch (band) {
	case NL80211_BAND_2GHZ:
		if (chan == 14)
			return 2484;
		else if (chan < 14)
			return 2407 + chan * 5;
		break;
	case NL80211_BAND_5GHZ:
		if (chan >= 182 && chan <= 196)
			return 4000 + chan * 5;
		else
			return 5000 + chan * 5;
		break;
	case NL80211_BAND_60GHZ:
		if (chan < 5)
			return 56160 + chan * 2160;
		break;
	default:
		;
	}
	return 0; /* не поддерживается */
}




int main(int argc, char **argv)
{
	
	int err,i;
	char device[]="wlan0"; //определяем переменную устройства
	unsigned int freq;
	unsigned int htval = NL80211_CHAN_HT20;
	int dtim_period=2; //переменная указывает временной интервал, по истечении которого широковещательные и многоадресные пакеты, помещенные в буфер, будут доставлены беспроводным клиентам
	struct nl_msg *msg;
	struct nl_cb *cb;

	/*Данные, необходимые для построения маяка (beacon)*/
	struct ieee80211_mgmt *head = NULL; //
	u8 *tail = NULL;
	size_t head_len = 0, tail_len = 0;
	u8 *pos, *tailpos;
	#define BEACON_HEAD_BUF_SIZE 256
	#define BEACON_TAIL_BUF_SIZE 512
	/**/
	
	err = nl80211_init(&nlstate); // при помощи функции nl80211_init инициализируем устройство
	
	memset(stations, 0, sizeof(struct station_data)); // заполняем массив stations нулями
	
	used_rates[0].rate = 10;               //инициализируем структуру used_rates
	used_rates[0].flags=RATE_BASIC;
	used_rates[1].rate = 20;
	used_rates[1].flags = RATE_BASIC;
	used_rates[2].rate = 55;
	used_rates[2].flags = RATE_BASIC;
	used_rates[3].rate = 110;
	used_rates[3].flags = RATE_BASIC;
	used_rates[4].rate = 60;
	used_rates[4].flags = 0;
	used_rates[5].rate = 90;
	used_rates[5].flags = 0;
	used_rates[6].rate = 120;
	used_rates[6].flags = 0;
	used_rates[7].rate = 180;
	used_rates[7].flags = 0;


	/*Переводим устройство в режим точки доступа*/
	devidx = if_nametoindex(device);	
	printf("::Setting device '%s' with devidx '%lld' into AP mode.\n",device, devidx);
	
	msg = nlmsg_alloc(); //определяем сообщение netlink при помощи функции nlmsg_alloc
	cb = nl_cb_alloc(NL_CB_DEFAULT);  //при помощи функции nl_cb_alloc мы выделяем дескриптор обратного вызова
	genlmsg_put(msg, 0, 0, nlstate.nl80211_id, 0, 0, NL80211_CMD_SET_INTERFACE, 0);
	NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);
	NLA_PUT_U32(msg, NL80211_ATTR_IFTYPE, NL80211_IFTYPE_AP);
	err = nl_send_auto_complete(nlstate.nl_sock, msg);
	nl_cb_err(cb, NL_CB_CUSTOM, error_handler, &err);
	nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &err);//при помощи функции nl_cb_set
	nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &err);      //устанавливаем следующие обратные вызовы
	err = 1;
	while (err > 0)
	{
		nl_recvmsgs(nlstate.nl_sock, cb);
		if(err!=0)
		{
			printf("Could not set device into AP mode error:%d\n",err);
		}
	}
	nl_cb_put(cb);
	nlmsg_free(msg);
	
		
	/*устанавливаем канал устройства*/
	printf("::Setting device '%s' with devidx '%lld' to channel %d.\n",device, devidx, channel);
	msg = nlmsg_alloc();
	cb = nl_cb_alloc(NL_CB_DEFAULT);
	genlmsg_put(msg, 0, 0, nlstate.nl80211_id, 0, 0, NL80211_CMD_SET_WIPHY, 0);
	NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);
	freq = ieee80211_channel_to_frequency(channel, NL80211_BAND_2GHZ);
	NLA_PUT_U32(msg, NL80211_ATTR_WIPHY_FREQ, freq);
	NLA_PUT_U32(msg, NL80211_ATTR_WIPHY_CHANNEL_TYPE, htval);
	err = nl_send_auto_complete(nlstate.nl_sock, msg);
	err = 1;
	nl_cb_err(cb, NL_CB_CUSTOM, error_handler, &err);
	nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &err);
	nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &err);

	while (err > 0)
	{
		nl_recvmsgs(nlstate.nl_sock, cb);
		if(err!=0)
		{
			printf("Could not set channel of device, error:%d\n",err);
		}
	}
	nl_cb_put(cb);
	nlmsg_free(msg);
		
			
	/*Регистрируем все необходимые подтипы кадров управления (определены стандартом IEEE802.11)*/
	printf("::Registering to all mgmt frames\n");
	struct nl_cb *cb_mgmt;
	cb_mgmt = nl_cb_alloc(NL_CB_DEFAULT); //при помощи функции nl_cb_alloc мы выделяем дескриптор обрытного вызова
	struct nl_sock *sock_mgmt;
	u16 type;
	const u8 *match=NULL;
	size_t match_len=0;
		static const int stypes[] = {
		WLAN_FC_STYPE_AUTH,
		WLAN_FC_STYPE_ASSOC_REQ,
		WLAN_FC_STYPE_REASSOC_REQ,
		WLAN_FC_STYPE_DISASSOC,
		WLAN_FC_STYPE_DEAUTH,
		WLAN_FC_STYPE_ACTION,
		WLAN_FC_STYPE_PROBE_REQ,
	};
	sock_mgmt = nl_socket_alloc_cb(cb_mgmt);
	if (sock_mgmt == NULL) {
		printf("Failed to allocate netlink callbacks\n");
	}
	if (genl_connect(sock_mgmt)) {
		printf("Failed to connect to generic netlink\n");
	}
	for (i = 0; i < 7; i++) 
	{
		type =(WLAN_FC_TYPE_MGMT << 2) | (stypes[i] << 4);
		msg = nlmsg_alloc();
		cb = nl_cb_alloc(NL_CB_DEFAULT);
		genlmsg_put(msg, 0, 0, nlstate.nl80211_id,
				   0, 0, NL80211_CMD_REGISTER_ACTION, 0);
		NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);
		NLA_PUT_U16(msg, NL80211_ATTR_FRAME_TYPE, type);
		NLA_PUT(msg, NL80211_ATTR_FRAME_MATCH, match_len, match);
		err = nl_send_auto_complete(sock_mgmt, msg);
		err = 1;
		nl_cb_err(cb_mgmt, NL_CB_CUSTOM, error_handler, &err);
		nl_cb_set(cb_mgmt, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, 
			&err);
		nl_cb_set(cb_mgmt, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &err);
		nl_cb_set(cb_mgmt, NL_CB_VALID, NL_CB_CUSTOM,process_mgmt_handler, 
			NULL);
		nl_cb_set(cb_mgmt, NL_CB_INVALID, NL_CB_CUSTOM, 
			mesgmal_handler, &err);
		printf("Register frame type=0x%x \n",type);
		
		nl_cb_put(cb);
		nlmsg_free(msg);
	}
	
	/*Устанавливаем параметры beacon и запускаем Точку Доступа*/
	printf("::Set beacon frame parameters and start AP\n");
	msg = nlmsg_alloc();
	cb = nl_cb_alloc(NL_CB_DEFAULT);
	genlmsg_put(msg, 0, 0, nlstate.nl80211_id, 0, 0, 
		NL80211_CMD_START_AP, 0);
	
	/*Поднимаем beacon*/
	head = malloc(BEACON_HEAD_BUF_SIZE);
	memset(head, 0, BEACON_HEAD_BUF_SIZE);
	tail_len = BEACON_TAIL_BUF_SIZE;
	tailpos = tail = malloc(tail_len);
	head->frame_control = IEEE80211_FC(WLAN_FC_TYPE_MGMT,
					   WLAN_FC_STYPE_BEACON);
	head->duration = host_to_le16(0);
	memset(head->da, 0xff, ETH_ALEN);
	memcpy(head->sa, ether, ETH_ALEN);
	memcpy(head->bssid, ether, ETH_ALEN);
	head->u.beacon.beacon_int =	host_to_le16(beacon_int);
	int capab = WLAN_CAPABILITY_ESS;
	head->u.beacon.capab_info = host_to_le16(capab);
	pos = &head->u.beacon.variable[0];
	/*Устанавливаем определенный ранее SSID*/
	*pos++ = WLAN_EID_SSID;
	*pos++ = strlen(ssid);
	memcpy(pos, ssid, strlen(ssid));
	pos += strlen(ssid);
	/*Supported rates*/
	*pos++ = WLAN_EID_SUPP_RATES;
	*pos++ = 8; //# of rates
	for (i=0; i<8; i++) 
	{
		*pos = used_rates[i].rate / 5;
		if (used_rates[i].flags & RATE_BASIC)
			*pos |= 0x80;
		pos++;
	}
	/*Параметр DS (текущий канал)*/
	*pos++ = WLAN_EID_DS_PARAMS;
	*pos++ = 1;
	*pos++ = channel;
	/*Вычислем длину заголовка*/
	head_len = pos - (u8 *) head;
	tail_len =0;
	
	NLA_PUT(msg, NL80211_ATTR_BEACON_HEAD, head_len, head);
	NLA_PUT(msg, NL80211_ATTR_BEACON_TAIL, tail_len, tail);
	NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);
	NLA_PUT_U32(msg, NL80211_ATTR_BEACON_INTERVAL, beacon_int);
	NLA_PUT_U32(msg, NL80211_ATTR_DTIM_PERIOD, dtim_period);
	NLA_PUT(msg, NL80211_ATTR_SSID, strlen(ssid), ssid);	
	NLA_PUT_U32(msg, NL80211_ATTR_HIDDEN_SSID,
			    NL80211_HIDDEN_SSID_NOT_IN_USE);    
	NLA_PUT_FLAG(msg, NL80211_ATTR_PRIVACY);
	NLA_PUT_U32(msg, NL80211_ATTR_AUTH_TYPE,
			    NL80211_AUTHTYPE_OPEN_SYSTEM);
	err = nl_send_auto_complete(nlstate.nl_sock, msg);
	nl_cb_err(cb, NL_CB_CUSTOM, error_handler, &err);
	nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &err);
	nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &err);
	err = 1;
	while (err > 0)
	{
		nl_recvmsgs(nlstate.nl_sock, cb);
		if(err!=0)
		{
			printf("Beacon Setup and AP Setup failed, error:%d\n",err);
		}
	}
	nl_cb_put(cb);
	nlmsg_free(msg);

	printf("::Get and process mgmt frames in while loop\n");

	while(true)
	{
		int res = nl_recvmsgs(sock_mgmt, cb_mgmt);
		if(err!=0)
		{
			printf("Error while processing mgmt frames: %d\n",err);
		}
	}

	return 0;
	
nla_put_failure:
	printf("NLA_PUT_FAILURE\n");
	return -ENOBUFS;	
}
