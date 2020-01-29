//
//  cmd.c
//  com.leith.appFirewall-Helper
//
//  Created by Doug Leith on 18/01/2020.
//  Copyright © 2020 Doug Leith. All rights reserved.
//

#include "cmd.h"

static int c_sock=-1;
static pthread_t cmd_thread, dns_thread;
static int dnscrypt_proxy_running=0;
static char dnscrypt_cmd[STR_SIZE], dnscrypt_arg[STR_SIZE];

void* dnscrypt(void* ptr) {
	int pid = 0;
	FILE *out = run_cmd_pipe(dnscrypt_cmd,dnscrypt_arg,&pid);
	char *resp=NULL; size_t llen = 0;
	ssize_t res=0;
	int tries = 0;
	while (dnscrypt_proxy_running) {
		res=getline(&resp, &llen, out);
		if (res == -1) {
			if (errno==EINTR) continue; // interrupted by signal
			if (feof(out)) {
				WARN("Problem reading dnscrypt-proxy output: end of file\n");
			} else {
				WARN("Problem reading dnscrypt-proxy output: %s\n",strerror(errno));
			}
			tries++;
			// we give it a few goes before stopping
			// since loss of dns server is pretty bad
			if (tries>10) break; // bail
			// restart and try again
			char cmd[STR_SIZE]; snprintf(cmd,STR_SIZE,"/bin/kill -9 %d",pid);
			if (run_cmd(cmd,CMD_TIMEOUT)!=0) { break; } // an error running cmd, bail
			fclose(out);
			out = run_cmd_pipe(dnscrypt_cmd,dnscrypt_arg,&pid);
			continue;
		}
		tries = 0; // reset counter
		printf("dnscrypt-proxy: %s",resp);
	}
	// stop service
	if (dnscrypt_proxy_running) {
		ERR("dnscrypt_proxy stopped prematurely: %s\n",strerror(errno));
	} else {
		printf("Dnscrypt-proxy thread stopped.\n");
	}
	char cmd[STR_SIZE]; snprintf(cmd,STR_SIZE,"/bin/kill -9 %d",pid);
	run_cmd(cmd,CMD_TIMEOUT); // nothing much we can do if this fails
	fclose(out); free(resp);
	return NULL;
}

int set_dns_server(char* dns) {
	// get list of network services
	FILE *out = popen("/usr/sbin/networksetup -listallnetworkservices","r");
	if (out==NULL) {
		WARN("Problem getting list of network services in set_dns_server(): %s\n", strerror(errno));
		pclose(out);
		return -3;
	}
	int count = 0, res = 0;
	int failedforsome=0, okforsome=0;
	char service[STR_SIZE], cmd_str[STR_SIZE];
	while ((res=readline_timed(service, STR_SIZE, out,CMD_TIMEOUT))>0) {
		count++;
		if (count==1) continue; // ignore first line of output
		char *tmp = trimwhitespace(service);
		// this command seems to hang intermittently, but run_cmd() times out
		snprintf(cmd_str,STR_SIZE,"/usr/sbin/networksetup setdnsservers \"%s\" \"%s\"",tmp,dns);
		printf("setting dns server for %s to %s\n",tmp,dns);
		int res2=0;
		if ( (res2=run_cmd(cmd_str,CMD_TIMEOUT))==-2) {
			// timedout, try again
			res2=run_cmd(cmd_str,CMD_TIMEOUT);
		}
		if (res2<0) {
			failedforsome++; // keep a log of the failure
		} else {
			okforsome++; // keep a log of success
		}
		// if this cmd fails, just continue to next network service
	}
	printf("set_dns_server() finished\n");
	pclose(out);
	return okforsome;
}

void* cmd_accept_loop(void* ptr) {
	// now wait in accept() loop to handle connections from GUI to send do stuff
	ssize_t res=0;
	struct sockaddr_in remote;
	socklen_t len = sizeof(remote);
	int s2;
	for(;;) {
		INFO("Waiting to accept connection on localhost port %d (send_rst)...\n", CMD_PORT);
		if ((s2 = accept(c_sock, (struct sockaddr *)&remote, &len)) == -1) {
			ERR("Problem accepting new connection on localhost port %d (send_rst): %s\n", CMD_PORT, strerror(errno));
			continue;
		}
		INFO("Started new connection on port %d (recv_cmd)\n", CMD_PORT);
		if (check_signature(s2, CMD_PORT)<0) {
			// couldn't authenticate client
			close(s2);
			continue;
		}
		int pid = get_sock_pid(s2, CMD_PORT);
		
		// when UI starts up it creates a connection and keeps it open
		// until it shuts down, so we accept and then keep listening
		// until other side closes (or we get an error).
		for(;;) {
			// before reading data, we recheck client when PID changes
			int current_pid = get_sock_pid(s2, CMD_PORT);
			if (current_pid != pid) {
				if (check_signature(s2, CMD_PORT)<0) break;
			}
			pid = current_pid;
			uint8_t cmd; int8_t ok=0;
			char src[STR_SIZE], dst[STR_SIZE], cmd_str[STR_SIZE];
			size_t src_len=0, dst_len=0;
			int tries=0;
			if ( (res=readn(s2, &cmd, 1) )<=0) break;
			switch (cmd) {
				case IntallUpdatecmd:
					// install update
					printf("Received install update command\n");
					if ( (res=readn(s2, &src_len, sizeof(size_t)) )<=0) break;
					if ((src_len<0) || (src_len>STR_SIZE)) break;
					memset(src,0,STR_SIZE);
					if ( (res=readn(s2, src, src_len) )<=0) break;
					if ( (res=readn(s2, &dst_len, sizeof(size_t)) )<=0) break;
					if ((dst_len<0) || (dst_len>STR_SIZE)) break;
					memset(dst,0,STR_SIZE);
					if ( (res=readn(s2, dst, dst_len) )<=0) break;
					printf("Received install update command parameters: src %s, dst %s\n",src,dst);
					
					// values passed in are the src and dst directories, so add filename
					// (use a fixed filename to add a bit of extra safety since running as root)
					strlcat(src,"/appFirewall.app",STR_SIZE);
					strlcat(dst,"/appFirewall.app",STR_SIZE);
					
					char *rm = "/bin/rm", *mv = "/bin/mv";
					snprintf(cmd_str,STR_SIZE,"%s -rf %s.bak",rm,dst);
					printf("install update do: %s\n",cmd_str);
					if ((res=run_cmd(cmd_str,LONG_CMD_TIMEOUT))!=0) {
						WARN("Problem executing command %s: %s (res=%zd)", cmd_str, strerror(errno), res);
						ok = -1;
						break; // not really fatal, but failure might be a symptom of something serious
					}
					// keep copy of existing app
					snprintf(cmd_str,STR_SIZE,"%s %s %s.bak",mv,dst,dst);
					printf("install update do: %s\n",cmd_str);
					if ((res=run_cmd(cmd_str,LONG_CMD_TIMEOUT))!=0) {
						WARN("Problem executing command %s: %s (res=%zd)", cmd_str, strerror(errno), res);
						ok = -2;
						break;
					}
					// install new app
					snprintf(cmd_str,STR_SIZE,"%s '%s' %s",mv,src,dst);
					printf("install update do: %s\n",cmd_str);
					if ((res=run_cmd(cmd_str,LONG_CMD_TIMEOUT))!=0) {
						WARN("Problem installing update using %s: %s (res=%zd)", cmd_str, strerror(errno), res);
						// try to restore old app
						snprintf(cmd_str,STR_SIZE,"%s %s.bak %s",mv,dst,dst);
						run_cmd(cmd_str,LONG_CMD_TIMEOUT); // if this fails there's not much we can do to recover !
						ok = -3;
						break;
					}
					// successful install, tidy up
					snprintf(cmd_str,STR_SIZE,"%s -rf %s.bak",rm, dst);
					printf("install update do: %s\n",cmd_str);
					if ((res=run_cmd(cmd_str,LONG_CMD_TIMEOUT))!=0) {
						WARN("Problem removing backup with command %s: %s (res=%zd)", cmd_str, strerror(errno), res);
						// not fatal ?
					}
					printf("install update successful\n");
					ok = 1;
					break;
				case BlockQUICcmd:
					//sudo pfctl -a com.apple/appFirewall -s rules // list rules
					printf("Received block QUIC command\n");
					snprintf(cmd_str,STR_SIZE,"/bin/echo \"block drop quick proto udp from any to any port 443\" | /sbin/pfctl -a com.apple/appFirewall -f -");
					printf("block QUIC do: %s\n",cmd_str);
					if ((res=run_cmd(cmd_str,CMD_TIMEOUT)) != 0) {
						WARN("Problem blocking QUIC, res=%zd\n",res);
						break;
					}
					printf("Block QUIC command successful\n");
					ok = 1;
					break;
				case UnblockQUICcmd:
					printf("Received unblock QUIC command\n");
					if ((res=run_cmd("/sbin/pfctl -a com.apple/appFirewall -F rules", CMD_TIMEOUT)) != 0) {
						WARN("Problem unblocking QUIC, res=%zd\n",res);
						break;
					}
					printf("Unblock QUIC command successful\n");
					ok = 1;
					break;
				case StartDNScmd:
					printf("Received start dnscrypt-proxy command\n");
					if ( (res=readn(s2, &src_len, sizeof(size_t)) )<=0) break;
					if ((src_len<0) || (src_len>STR_SIZE)) break;
					memset(src,0,STR_SIZE);
					if ( (res=readn(s2, src, src_len) )<=0) break;
					if (!dnscrypt_proxy_running) {
						// TO DO: check signature of executable
						snprintf(dnscrypt_cmd,STR_SIZE,"%s/dnscrypt-proxy",src);
						snprintf(dnscrypt_arg,STR_SIZE,"-config=%s/dnscrypt-proxy.toml",src);
						printf("cmd=%s %s\n",dnscrypt_cmd, dnscrypt_arg);
						pthread_create(&dns_thread, NULL, dnscrypt, NULL);
						dnscrypt_proxy_running=1;
						printf("start dnscrypt-proxy completed.\n");
					} else {
						printf("start dnscrypt-proxy: already running\n");
					}
					printf("Set DNS server to localhost\n");
					// we try a few times as seems prone to timing out
					while (((res=set_dns_server("127.0.0.1"))==0) && (tries<5)){tries++;}
					if (res>0)
						ok = 1; // success;
					else
						ok = -1;
					printf("set DNS server to 127.0.0.1 completed\n");
					break;
				case StopDNScmd:
					printf("Received stop dnscrypt-proxy command\n");
					printf("Set DNS server to default\n");
					while (( (res=set_dns_server("empty"))==0) && (tries<5)){tries++;}
					if (res>0)
						ok = 1; // success;
					else {
						ok = -1;
						// try to rollback ...
						set_dns_server("127.0.0.1");
						break; // bail here, since interfaces still using dnscrypt-proxy
					}
					printf("set DNS server to default completed\n");
					printf("Stopping dnscrypt-proxy ...\n");
					if (dnscrypt_proxy_running) {
						dnscrypt_proxy_running=0;
						// interrupt getline() in dns_thread so it checks
						// dnscrypt_proxy_running flag
						pthread_kill(dns_thread, SIGUSR1);
						printf("stop dnscrypt-proxy signalled.\n");
					} else {
						printf("stop dnscrypt-proxy: not running.\n");
					}
					break;
				default:
					WARN("Unexpected command received: %d\n", cmd);
					break; // close connection
			}
			// send response back
			set_snd_timeout(s2, SND_TIMEOUT); // to be safe, will eventually timeout of send
			if (send(s2, &ok, 1, 0)<=0) break;
		}
		// likely UI client has closed its end of the connection, in which
		// case res=0, otherwise something worse has happened to connection
		if (res<0) WARN("recv() on port %d (recv_cmd): %s\n",CMD_PORT, strerror(errno));
		INFO("Connection closed on port %d (recv_cmd).\n", CMD_PORT);
		close(s2);
	}
}

void start_cmd() {
	// start listening for requests to do stuff
	c_sock = bind_to_port(CMD_PORT,2);
	INFO("Now listening on localhost port %d (%s)\n", CMD_PORT, "recv_cmd");
	pthread_create(&cmd_thread, NULL, cmd_accept_loop, NULL);
}
