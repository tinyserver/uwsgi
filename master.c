#include "uwsgi.h"

extern struct uwsgi_server uwsgi;

void expire_rb_timeouts(struct rb_root *root) {

        time_t current = time(NULL);
        struct uwsgi_rb_timer *urbt;
        struct uwsgi_signal_rb_timer *usrbt;

        for(;;) {

                urbt = uwsgi_min_rb_timer(root);

                if (urbt == NULL) return;

                if (urbt->key <= current) {
			// remove the timeout and add another
			usrbt = (struct uwsgi_signal_rb_timer *) urbt->data;
			rb_erase(&usrbt->uwsgi_rb_timer->rbt, root);
			free(usrbt->uwsgi_rb_timer);
			usrbt->iterations_done++;
			uwsgi_route_signal(usrbt->sig);
			if (!usrbt->iterations || usrbt->iterations_done < usrbt->iterations) {
				usrbt->uwsgi_rb_timer = uwsgi_add_rb_timer(root, time(NULL) + usrbt->value, usrbt);
			}
                        continue;
                }

                break;
        }
}


void uwsgi_subscribe(char *subscription) {

	char *ssb;
	char subscrbuf[4096];
	uint16_t ustrlen;

	char *udp_address = strchr(subscription,':');
        if (!udp_address) return;

        char *subscription_key = strchr(udp_address+1, ':');
        udp_address = uwsgi_concat2n(subscription, subscription_key-subscription, "", 0);

        ssb = subscrbuf;

	ustrlen = 3;
        *ssb++ = (uint8_t) (ustrlen  & 0xff);
        *ssb++ = (uint8_t) ((ustrlen >>8) & 0xff);
        memcpy(ssb, "key", ustrlen);
        ssb+=ustrlen;

        ustrlen = strlen(subscription_key+1);
        *ssb++ = (uint8_t) (ustrlen  & 0xff);
        *ssb++ = (uint8_t) ((ustrlen >>8) & 0xff);
        memcpy(ssb, subscription_key+1, ustrlen);
        ssb+=ustrlen;

        ustrlen = 7;
        *ssb++ = (uint8_t) (ustrlen  & 0xff);
        *ssb++ = (uint8_t) ((ustrlen >>8) & 0xff);
        memcpy(ssb, "address", ustrlen);
        ssb+=ustrlen;

        ustrlen = strlen(uwsgi.sockets[0].name);
        *ssb++ = (uint8_t) (ustrlen  & 0xff);
        *ssb++ = (uint8_t) ((ustrlen >>8) & 0xff);
        memcpy(ssb, uwsgi.sockets[0].name, ustrlen);
        ssb+=ustrlen;

        send_udp_message(224, udp_address, subscrbuf, ssb-subscrbuf);
	free(udp_address);

}

#ifdef __linux__
void get_linux_tcp_info(int fd) {
	socklen_t tis = sizeof(struct tcp_info);

	if (!getsockopt(fd, IPPROTO_TCP, TCP_INFO, &uwsgi.shared->ti, &tis)) {
		// a check for older linux kernels
		if (!uwsgi.shared->ti.tcpi_sacked) {
			return;
		}
		if (uwsgi.shared->ti.tcpi_unacked >= uwsgi.shared->ti.tcpi_sacked) {
			uwsgi_log_verbose("*** uWSGI listen queue of socket %d full !!! (%d/%d) ***\n", fd, uwsgi.shared->ti.tcpi_unacked, uwsgi.shared->ti.tcpi_sacked);
		}
	}
}
#endif

void manage_cluster_announce(char *key, uint16_t keylen, char *val, uint16_t vallen, void *data) {

	char *tmpstr;
	struct uwsgi_cluster_node *ucn = (struct uwsgi_cluster_node *) data;
	uwsgi_log("%.*s = %.*s\n", keylen, key, vallen, val);

	if (!uwsgi_strncmp("hostname", 8, key, keylen)) {
		strncpy(ucn->nodename, val, UMIN(vallen, 255));
	}

	if (!uwsgi_strncmp("address", 7, key, keylen)) {
		strncpy(ucn->name, val, UMIN(vallen, 100));
	}

	if (!uwsgi_strncmp("workers", 7, key, keylen)) {
		tmpstr = uwsgi_concat2n(val, vallen, "", 0);
		ucn->workers = atoi(tmpstr);
		free(tmpstr);
	}

	if (!uwsgi_strncmp("requests", 8, key, keylen)) {
		tmpstr = uwsgi_concat2n(val, vallen, "", 0);
		ucn->requests = strtoul(tmpstr, NULL, 0);
		free(tmpstr);
	}
}

void master_loop(char **argv, char **environ) {

	uint64_t master_cycles = 0;
	uint64_t tmp_counter;

	char log_buf[4096];

	uint64_t current_time = time(NULL);

	struct timeval last_respawn;

	pid_t pid;
	int pid_found = 0;

	pid_t diedpid;
	int waitpid_status;

	int working_workers = 0;
	int blocking_workers = 0;

	int ready_to_reload = 0;
	int ready_to_die = 0;

	int master_has_children = 0;

	uint8_t uwsgi_signal;

#ifdef UWSGI_UDP
	struct pollfd uwsgi_poll[2];
	int uwsgi_poll_size = 0;
	struct sockaddr_in udp_client;
	socklen_t udp_len;
	char udp_client_addr[16];
	int udp_managed = 0;
	int udp_fd = -1 ;

#ifdef UWSGI_MULTICAST
	char *cluster_opt_buf = NULL;
	int cluster_opt_size = 4;

	char *cptrbuf;
	uint16_t ustrlen;
	struct uwsgi_header *uh;
	struct uwsgi_cluster_node nucn;
#endif
#endif

	int i=0,j;
	int rlen;

	int check_interval = 1;

	struct uwsgi_rb_timer *min_timeout;
	struct rb_root *rb_timers = uwsgi_init_rb_timer();

	// release the GIL
	//UWSGI_RELEASE_GIL

	/* route signals to workers... */
	signal(SIGHUP, (void *) &grace_them_all);
	signal(SIGTERM, (void *) &reap_them_all);
	signal(SIGINT, (void *) &kill_them_all);
	signal(SIGQUIT, (void *) &kill_them_all);
	/* used only to avoid human-errors */

	signal(SIGUSR1, (void *) &stats);

	uwsgi.master_queue = event_queue_init();

	uwsgi_log("adding %d to signal poll\n", uwsgi.shared->worker_signal_pipe[0]);
	event_queue_add_fd_read(uwsgi.master_queue, uwsgi.shared->worker_signal_pipe[0]);

	if (uwsgi.log_master) {
		uwsgi_log("adding %d to master logging\n", uwsgi.shared->worker_log_pipe[0]);
		event_queue_add_fd_read(uwsgi.master_queue, uwsgi.shared->worker_log_pipe[0]);
	}
	

	uwsgi.wsgi_req->buffer = uwsgi.async_buf[0];

	if (uwsgi.has_emperor) {
		event_queue_add_fd_read(uwsgi.master_queue, uwsgi.emperor_fd);
	}
#ifdef UWSGI_UDP
	if (uwsgi.udp_socket) {
		udp_fd = bind_to_udp(uwsgi.udp_socket, 0, 0);
		uwsgi_poll[uwsgi_poll_size].fd = udp_fd;
		if (uwsgi_poll[uwsgi_poll_size].fd < 0) {
			uwsgi_log( "unable to bind to udp socket. SNMP and cluster management services will be disabled.\n");
		}
		else {
			uwsgi_log( "UDP server enabled.\n");
			uwsgi_poll[uwsgi_poll_size].events = POLLIN;
			uwsgi_poll_size++;
		}
	}
#ifdef UWSGI_MULTICAST
	if (uwsgi.cluster) {
		uwsgi_poll[uwsgi_poll_size].fd = uwsgi.cluster_fd;
		uwsgi_poll[uwsgi_poll_size].events = POLLIN;
		uwsgi_poll_size++;

		event_queue_add_fd_read(uwsgi.master_queue, uwsgi.cluster_fd);

		for(i=0;i<uwsgi.exported_opts_cnt;i++) {
			//uwsgi_log("%s\n", uwsgi.exported_opts[i]->key);
                	cluster_opt_size += 2+strlen(uwsgi.exported_opts[i]->key);
			if (uwsgi.exported_opts[i]->value) {
                        	cluster_opt_size += 2+strlen(uwsgi.exported_opts[i]->value);
			}
			else {
				cluster_opt_size += 2 + 1;
			}
                }

		//uwsgi_log("cluster opts size: %d\n", cluster_opt_size);
		cluster_opt_buf = uwsgi_malloc(cluster_opt_size);

		uh = (struct uwsgi_header *) cluster_opt_buf;

		uh->modifier1 = 99;
		uh->pktsize = cluster_opt_size - 4;
		uh->modifier2 = 1;
	
		cptrbuf = cluster_opt_buf+4;

		for(i=0;i<uwsgi.exported_opts_cnt;i++) {
			//uwsgi_log("%s\n", uwsgi.exported_opts[i]->key);
			ustrlen = strlen(uwsgi.exported_opts[i]->key);
			*cptrbuf++ = (uint8_t) (ustrlen	 & 0xff);
			*cptrbuf++ = (uint8_t) ((ustrlen >>8) & 0xff);
			memcpy(cptrbuf, uwsgi.exported_opts[i]->key, ustrlen);
			cptrbuf+=ustrlen;

			if (uwsgi.exported_opts[i]->value) {
				ustrlen = strlen(uwsgi.exported_opts[i]->value);
				*cptrbuf++ = (uint8_t) (ustrlen	 & 0xff);
				*cptrbuf++ = (uint8_t) ((ustrlen >>8) & 0xff);
				memcpy(cptrbuf, uwsgi.exported_opts[i]->value, ustrlen);
			}
			else {
				ustrlen = 1;
				*cptrbuf++ = (uint8_t) (ustrlen	 & 0xff);
				*cptrbuf++ = (uint8_t) ((ustrlen >>8) & 0xff);
				*cptrbuf = '1' ;
			}
			cptrbuf+=ustrlen;
		}

	}
#endif
#endif

#ifdef UWSGI_SNMP
	if (uwsgi.snmp) {
		if (uwsgi.snmp_community) {
			if (strlen(uwsgi.snmp_community) > 72) {
				uwsgi_log( "*** warning the supplied SNMP community string will be truncated to 72 chars ***\n");
				memcpy(uwsgi.shared->snmp_community, uwsgi.snmp_community, 72);
			}
			else {
				memcpy(uwsgi.shared->snmp_community, uwsgi.snmp_community, strlen(uwsgi.snmp_community) + 1);
			}
		}
		uwsgi_log( "filling SNMP table...");

		uwsgi.shared->snmp_gvalue[0].type = SNMP_COUNTER64;
		uwsgi.shared->snmp_gvalue[0].val = &uwsgi.workers[0].requests;

		uwsgi_log( "done\n");

	}
#endif

	
/*


*/

	// spawn fat gateways
	for(i=0;i<uwsgi.gateways_cnt;i++) {
        	if (uwsgi.gateways[i].pid == 0) {
                	gateway_respawn(i);
                }
        }

	// first subscription
	for(i=0;i<uwsgi.subscriptions_cnt;i++) {
		uwsgi_log("requested subscription for %s\n", uwsgi.subscriptions[i]);
		uwsgi_subscribe(uwsgi.subscriptions[i]);
	}

	for (;;) {
		//uwsgi_log("ready_to_reload %d %d\n", ready_to_reload, uwsgi.numproc);
		if (ready_to_die >= uwsgi.numproc && uwsgi.to_hell) {
#ifdef UWSGI_SPOOLER
			if (uwsgi.spool_dir && uwsgi.shared->spooler_pid > 0) {
				kill(uwsgi.shared->spooler_pid, SIGKILL);
				uwsgi_log( "killed the spooler with pid %d\n", uwsgi.shared->spooler_pid);
			}

#endif

			// TODO kill all the gateways
			uwsgi_log( "goodbye to uWSGI.\n");
			exit(0);
		}
		if (ready_to_reload >= uwsgi.numproc && uwsgi.to_heaven) {
#ifdef UWSGI_SPOOLER
			if (uwsgi.spool_dir && uwsgi.shared->spooler_pid > 0) {
				kill(uwsgi.shared->spooler_pid, SIGKILL);
				uwsgi_log( "wait4() the spooler with pid %d...", uwsgi.shared->spooler_pid);
				diedpid = waitpid(uwsgi.shared->spooler_pid, &waitpid_status, 0);
				uwsgi_log( "done.");
			}
#endif

#ifdef UWSGI_PROXY
			// TODO gracefully kill all the gateways (wait4 them) [see the spooler]
#endif
			uwsgi_log( "binary reloading uWSGI...\n");
			if (chdir(uwsgi.cwd)) {
				uwsgi_error("chdir()");
				exit(1);
			}
			/* check fd table (a module can obviosly open some fd on initialization...) */
			uwsgi_log( "closing all non-uwsgi socket fds > 2 (_SC_OPEN_MAX = %ld)...\n", sysconf(_SC_OPEN_MAX));
			for (i = 3; i < sysconf(_SC_OPEN_MAX); i++) {
				int found = 0;
				for(j=0;j<uwsgi.sockets_cnt;j++) {
					if (i == uwsgi.sockets[j].fd) {
						uwsgi_log("found fd %d\n", i);
						found = 1;
						break;
					}
				}

				if (!found) {
					if (uwsgi.has_emperor) {
						if (i == uwsgi.emperor_fd) {
							found = 1;
						}
					}
				}
				if (!found) {
					close(i);
				}
			}

			uwsgi_log( "running %s\n", uwsgi.binary_path);
			argv[0] = uwsgi.binary_path;
			//strcpy (argv[0], uwsgi.binary_path);
			execvp(uwsgi.binary_path, argv);
			uwsgi_error("execvp()");
			// never here
			exit(1);
		}

		if (uwsgi.numproc > 0 || uwsgi.gateways_cnt > 0 || ushared->daemons_cnt > 0) {
			master_has_children = 1;
		}
#ifdef UWSGI_SPOOLER
		if (uwsgi.spool_dir && uwsgi.shared->spooler_pid > 0) {
			master_has_children = 1;
		}
#endif
#ifdef UWSGI_PROXY
		if (uwsgi.proxy_socket_name && uwsgi.shared->proxy_pid > 0) {
			master_has_children = 1;
		}
		// TODO if gateways > 0 master_has_children == 1
#endif

		if (!master_has_children) {
			diedpid = 0;
		}
		else {
			diedpid = waitpid(WAIT_ANY, &waitpid_status, WNOHANG);
			if (diedpid == -1) {
				uwsgi_error("waitpid()");
				/* here is better to reload all the uWSGI stack */
				uwsgi_log( "something horrible happened...\n");
				reap_them_all();
				exit(1);
			}
		}

		if (diedpid == 0) {

			/* all processes ok, doing status scan after N seconds */
			check_interval = uwsgi.shared->options[UWSGI_OPTION_MASTER_INTERVAL];
			if (!check_interval)
				check_interval = 1;


			// add unregistered file monitors
			// locking is not needed as monitors can only increase
			for(i=0;i<ushared->files_monitored_cnt;i++) {
				if (!ushared->files_monitored[i].registered) {
					ushared->files_monitored[i].fd = event_queue_add_file_monitor(uwsgi.master_queue, ushared->files_monitored[i].filename, &ushared->files_monitored[i].id);
					ushared->files_monitored[i].registered = 1;		
				}
			}

			// add unregistered daemons
			// locking is not needed as daemons can only increase (for now)
			for(i=0;i<ushared->daemons_cnt;i++) {
				if (!ushared->daemons[i].registered) {
					uwsgi_log("spawning daemon %s\n", ushared->daemons[i].command);
					spawn_daemon(&ushared->daemons[i]);
					ushared->daemons[i].registered = 1;		
				}
			}


			// add unregistered timers
			// locking is not needed as timers can only increase
			for(i=0;i<ushared->timers_cnt;i++) {
                                if (!ushared->timers[i].registered) {
					ushared->timers[i].fd = event_queue_add_timer(uwsgi.master_queue, &ushared->timers[i].id, ushared->timers[i].value);
					ushared->timers[i].registered = 1;
				}
			}

			// add unregistered rb_timers
			// locking is not needed as rb_timers can only increase
			for(i=0;i<ushared->rb_timers_cnt;i++) {
                                if (!ushared->rb_timers[i].registered) {
					ushared->rb_timers[i].uwsgi_rb_timer = uwsgi_add_rb_timer(rb_timers, time(NULL) + ushared->rb_timers[i].value, &ushared->rb_timers[i]);
					ushared->rb_timers[i].registered = 1;
				}
			}

				int interesting_fd = -1;

				if (ushared->rb_timers_cnt>0) {
					min_timeout = uwsgi_min_rb_timer(rb_timers);
                			if (min_timeout == NULL ) {
                        			check_interval = uwsgi.shared->options[UWSGI_OPTION_MASTER_INTERVAL];
                			}
                			else {
                        			check_interval = min_timeout->key - time(NULL);
                        			if (check_interval <= 0) {
                                			expire_rb_timeouts(rb_timers);
                                			check_interval = 0;
                        			}
                			}
				}
				rlen = event_queue_wait(uwsgi.master_queue, check_interval, &interesting_fd);

				if (rlen == 0) {
					if (ushared->rb_timers_cnt>0) {
						expire_rb_timeouts(rb_timers);
					}
				}

				if (rlen > 0) {

					if (uwsgi.log_master) {
						if (interesting_fd == uwsgi.shared->worker_log_pipe[0]) {
							rlen = read(uwsgi.shared->worker_log_pipe[0], log_buf, 4096);
							if (rlen > 0) {
								if (uwsgi.log_syslog) {
									syslog(LOG_INFO, "%.*s", rlen, log_buf);
								}
								// TODO allow uwsgi.logger = func
							}	
						}
					}

					if (uwsgi.has_emperor) {
						if (interesting_fd == uwsgi.emperor_fd) {
							char byte;
							rlen = read(uwsgi.emperor_fd, &byte, 1);
                                                        if (rlen > 0) {
								uwsgi_log("received message %d from emperor\n", byte);
								// remove me
								if (byte == 0) {
									close(uwsgi.emperor_fd);
									uwsgi.has_emperor = 0;
									kill_them_all();
								}
								// reload me
								else if (byte == 1) {
									grace_them_all();
								}
                                                        }
							else {
								uwsgi_log("lost connection with my emperor !!!\n");
								close(uwsgi.emperor_fd);
								uwsgi.has_emperor = 0;
								kill_them_all();
							}
						}
					}

#ifdef UWSGI_UDP
					if (uwsgi.udp_socket && interesting_fd == udp_fd) {
						udp_len = sizeof(udp_client);
						rlen = recvfrom(udp_fd, uwsgi.wsgi_req->buffer, uwsgi.buffer_size, 0, (struct sockaddr *) &udp_client, &udp_len);

						if (rlen < 0) {
							uwsgi_error("recvfrom()");
						}
						else if (rlen > 0) {

							memset(udp_client_addr, 0, 16);
							if (inet_ntop(AF_INET, &udp_client.sin_addr.s_addr, udp_client_addr, 16)) {
								if (uwsgi.wsgi_req->buffer[0] == UWSGI_MODIFIER_MULTICAST_ANNOUNCE) {
								}
#ifdef UWSGI_SNMP
								else if (uwsgi.wsgi_req->buffer[0] == 0x30 && uwsgi.snmp) {
									manage_snmp(udp_fd, (uint8_t *) uwsgi.wsgi_req->buffer, rlen, &udp_client);
								}
#endif
								else {

									// loop the various udp manager until one returns true
									udp_managed = 0;
									for(i=0;i<0xFF;i++) {
										if (uwsgi.p[i]->manage_udp) {
											if (uwsgi.p[i]->manage_udp(udp_client_addr, udp_client.sin_port, uwsgi.wsgi_req->buffer, rlen)) {
												udp_managed = 1;
												break;
											}
										}
									}

									// else a simple udp logger
									if (!udp_managed) {
										uwsgi_log( "[udp:%s:%d] %.*s", udp_client_addr, ntohs(udp_client.sin_port), rlen, uwsgi.wsgi_req->buffer);
									}
								}
							}
							else {
								uwsgi_error("inet_ntop()");
							}
						}

						continue;
					}

#ifdef UWSGI_MULTICAST
					if (interesting_fd == uwsgi.cluster_fd) {
					
						if (uwsgi_get_dgram(uwsgi.cluster_fd, uwsgi.wsgi_requests[0])) {
							continue;
						}

						switch(uwsgi.wsgi_requests[0]->uh.modifier1) {
							case 95:
								memset(&nucn, 0, sizeof(struct uwsgi_cluster_node));
								uwsgi_hooked_parse(uwsgi.wsgi_requests[0]->buffer, uwsgi.wsgi_requests[0]->uh.pktsize, manage_cluster_announce, &nucn);
								if (nucn.name[0] != 0) {
									uwsgi_cluster_add_node(&nucn, CLUSTER_NODE_DYNAMIC);
								}
								break;
							case 96:
								uwsgi_log_verbose("%.*s\n", uwsgi.wsgi_requests[0]->uh.pktsize, uwsgi.wsgi_requests[0]->buffer);
								break;
							case 98:
								if (kill(getpid(), SIGHUP)) {
									uwsgi_error("kill()");
								}
								break;
							case 99:
								if (uwsgi.cluster_nodes) break;
								if (uwsgi.wsgi_requests[0]->uh.modifier2 == 0) {
									uwsgi_log("requested configuration data, sending %d bytes\n", cluster_opt_size);
									sendto(uwsgi.cluster_fd, cluster_opt_buf, cluster_opt_size, 0, (struct sockaddr *) &uwsgi.mc_cluster_addr, sizeof(uwsgi.mc_cluster_addr));
								}
								break;
							case 73:
								uwsgi_log_verbose("[uWSGI cluster %s] new node available: %.*s\n", uwsgi.cluster, uwsgi.wsgi_requests[0]->uh.pktsize, uwsgi.wsgi_requests[0]->buffer);
								break;
						}
						continue;
					}
#endif

#endif

					
					int next_iteration = 0;

					uwsgi_lock(uwsgi.fmon_table_lock);
					for(i=0;i<ushared->files_monitored_cnt;i++) {
						if (ushared->files_monitored[i].registered) {
							uwsgi_log("fmon check %d == %d\n", interesting_fd, ushared->files_monitored[i].fd) ;
							if (interesting_fd == ushared->files_monitored[i].fd) {
								struct uwsgi_fmon *uf = event_queue_ack_file_monitor(uwsgi.master_queue, interesting_fd);
								// now call the file_monitor handler
								if (uf) uwsgi_route_signal(uf->sig);
								break;
							}
						}
					}

					uwsgi_unlock(uwsgi.fmon_table_lock);
					if (next_iteration) continue;

					next_iteration = 0;

					for(i=0;i<ushared->timers_cnt;i++) {
                                                if (ushared->timers[i].registered) {
                                                        if (interesting_fd == ushared->timers[i].fd) {
                                                                struct uwsgi_timer *ut = event_queue_ack_timer(interesting_fd);
                                                                // now call the file_monitor handler
                                                                if (ut) uwsgi_route_signal(ut->sig);
                                                                break;
                                                        }
                                                }
                                        }
                                        if (next_iteration) continue;


					// check for worker signal
					if (interesting_fd == uwsgi.shared->worker_signal_pipe[0]) {
						rlen = read(interesting_fd, &uwsgi_signal, 1);
						if (rlen < 0) {
							uwsgi_error("read()");
						}	
						else if (rlen > 0) {
							uwsgi_log("received uwsgi signal %d from workers\n", uwsgi_signal);
							uwsgi_route_signal(uwsgi_signal);
						}
						else {
							uwsgi_log_verbose("lost connection with worker %d\n", i);
							close(interesting_fd);
							//uwsgi.workers[i].pipe[0] = -1;
						}
					}
				}

			current_time = time(NULL);	
			// checking logsize
			if (uwsgi.logfile) {
				uwsgi.shared->logsize = lseek(2, 0, SEEK_CUR);
				if (uwsgi.shared->logsize > 8192) {
					//uwsgi_log("logsize: %d\n", uwsgi.shared->logsize);
					char *new_logfile = uwsgi_malloc(strlen(uwsgi.logfile) + 14 + 1);
					memset(new_logfile, 0, strlen(uwsgi.logfile) + 14 + 1);    
					if (!rename(uwsgi.logfile, new_logfile)) {
						// close 2, reopen logfile dup'it and gracefully reload workers;
					}
					free(new_logfile);
				}	
			}

				
			master_cycles++;
			working_workers = 0;
			blocking_workers = 0;

			// recalculate requests counter on race conditions risky configurations
			// a bit of inaccuracy is better than locking;)

			if (uwsgi.cores > 1) {
				for(i=1;i<uwsgi.numproc+1;i++) {
					tmp_counter = 0;
					for(j=0;j<uwsgi.cores;j++) {
						tmp_counter += uwsgi.core[j]->requests;
					}
					uwsgi.workers[i].requests = tmp_counter;
				}
			}

			if (uwsgi.numproc > 1) {
				tmp_counter = 0;
				for(i=1;i<uwsgi.numproc+1;i++) {
					tmp_counter += uwsgi.workers[i].requests;
				}
				uwsgi.workers[0].requests = tmp_counter;
			}

			// remove expired cache items
			if (uwsgi.cache_max_items > 0) {
				for(i=0;i< (int)uwsgi.cache_max_items;i++) {
					uwsgi_wlock(uwsgi.cache_lock);
					if (uwsgi.cache_items[i].expires) {
						if (uwsgi.cache_items[i].expires < current_time) {
							uwsgi_cache_del(uwsgi.cache_items[i].key, uwsgi.cache_items[i].keysize);
						}
					}
					uwsgi_rwunlock(uwsgi.cache_lock);
				}
			}

			check_interval = uwsgi.shared->options[UWSGI_OPTION_MASTER_INTERVAL];
			if (!check_interval)
				check_interval = 1;


#ifdef __linux__
			for(i=0;i<uwsgi.sockets_cnt;i++) {
				if (uwsgi.sockets[i].family != AF_INET) continue;
				get_linux_tcp_info(uwsgi.sockets[i].fd);
			}
#endif

			for (i = 1; i <= uwsgi.numproc; i++) {
				/* first check for harakiri */
				if (uwsgi.workers[i].harakiri > 0) {
					if (uwsgi.workers[i].harakiri < (time_t) current_time) {
						/* first try to invoke the harakiri() custom handler */
						/* TODO */
						/* then brutally kill the worker */
						uwsgi_log("*** HARAKIRI ON WORKER %d (pid: %d) ***\n", i, uwsgi.workers[i].pid);
						if (uwsgi.harakiri_verbose) {
#ifdef __linux__
							int proc_file;
							char proc_buf[4096];
							char proc_name[64];
							ssize_t proc_len;

							if (snprintf(proc_name, 64, "/proc/%d/syscall", uwsgi.workers[i].pid) > 0) {
								memset(proc_buf, 0, 4096);
								proc_file = open(proc_name, O_RDONLY);
								if (proc_file >= 0) {
									proc_len = read(proc_file, proc_buf, 4096);
									if (proc_len > 0) {
										uwsgi_log("HARAKIRI: -- syscall> %s", proc_buf);
									}
									close(proc_file);	
								}
							}

							if (snprintf(proc_name, 64, "/proc/%d/wchan", uwsgi.workers[i].pid) > 0) {
								memset(proc_buf, 0, 4096);

								proc_file = open(proc_name, O_RDONLY);
                                                        	if (proc_file >= 0) {
                                                                	proc_len = read(proc_file, proc_buf, 4096);
                                                                	if (proc_len > 0) {
                                                                        	uwsgi_log("HARAKIRI: -- wchan> %s\n", proc_buf);
                                                                	}
                                                                	close(proc_file);
                                                        	}
							}
						
#endif
						}
						kill(uwsgi.workers[i].pid, SIGUSR2);
						// allow SIGUSR2 to be delivered
						sleep(1);
						kill(uwsgi.workers[i].pid, SIGKILL);
						// to avoid races
						uwsgi.workers[i].harakiri = 0;
					}
				}
				/* load counters */
				if (uwsgi.workers[i].status & UWSGI_STATUS_IN_REQUEST)
					working_workers++;

				if (uwsgi.workers[i].status & UWSGI_STATUS_BLOCKING)
					blocking_workers++;

				uwsgi.workers[i].last_running_time = uwsgi.workers[i].running_time;
			}

#ifdef UWSGI_UDP
			// check for cluster nodes
			for (i = 0; i < MAX_CLUSTER_NODES; i++) {
				struct uwsgi_cluster_node *ucn = &uwsgi.shared->nodes[i];

				if (ucn->name[0] != 0 && ucn->type == CLUSTER_NODE_STATIC && ucn->status == UWSGI_NODE_FAILED) {
					// should i retry ?
					if (master_cycles % ucn->errors == 0) {
						if (!uwsgi_ping_node(i, uwsgi.wsgi_req)) {
							ucn->status = UWSGI_NODE_OK;
							uwsgi_log( "re-enabled cluster node %d/%s\n", i, ucn->name);
						}
						else {
							ucn->errors++;
						}
					}
				}
				else if (ucn->name[0] != 0 && ucn->type == CLUSTER_NODE_DYNAMIC) {
					// if the last_seen attr is higher than 30 secs ago, mark the node as dead
					if ( (current_time - ucn->last_seen) > 30) {
						uwsgi_log_verbose("no presence announce in the last 30 seconds by node %s, i assume it is dead.\n", ucn->name);
						ucn->name[0] = 0 ;
					}
				}
			}

			// reannounce myself every 10 cycles
			if (uwsgi.cluster && uwsgi.cluster_fd >= 0 && !uwsgi.cluster_nodes && (master_cycles % 10) == 0) {
				uwsgi_cluster_add_me();
			}

			// resubscribe every 10 cycles
			if (uwsgi.subscriptions_cnt > 0 && (master_cycles % 10) == 0) {
				for(i=0;i<uwsgi.subscriptions_cnt;i++) {
					uwsgi_subscribe(uwsgi.subscriptions[i]);
				}
			}


#endif

			// now check for lb pool
			
			
			continue;

		}
#ifdef UWSGI_SPOOLER
		/* reload the spooler */
		if (uwsgi.spool_dir && uwsgi.shared->spooler_pid > 0) {
			if (diedpid == uwsgi.shared->spooler_pid) {
				uwsgi_log( "OOOPS the spooler is no more...trying respawn...\n");
				uwsgi.shared->spooler_pid = spooler_start();
				continue;
			}
		}
#endif

		// reload gateways and daemons only on normal workflow
		if (!uwsgi.to_heaven && !uwsgi.to_hell) {
		/* reload the gateways */
		// TODO reload_gateway(diedpid);
		pid_found = 0;
		for(i=0;i<uwsgi.gateways_cnt;i++) {
			if (uwsgi.gateways[i].pid == diedpid) {
				gateway_respawn(i);
				pid_found = 1;
				break;
			}
		}

		if (pid_found) continue;

		/* reload the daemons */
                // TODO reload_gateway(diedpid);
                pid_found = 0;
                for(i=0;i<uwsgi.shared->daemons_cnt;i++) {
                        if (uwsgi.shared->daemons[i].pid == diedpid) {
                                spawn_daemon(&uwsgi.shared->daemons[i]);
                                pid_found = 1;
                                break;
                        }
                }

		if (pid_found) continue;
		}

#ifdef UWSGI_PROXY
		if (uwsgi.proxy_socket_name && uwsgi.shared->proxy_pid > 0) {
			if (diedpid == uwsgi.shared->proxy_pid) {
				if (WIFEXITED(waitpid_status)) {
					if (WEXITSTATUS(waitpid_status) != UWSGI_END_CODE) {
						uwsgi_log( "OOOPS the proxy is no more...trying respawn...\n");
						uwsgi.shared->proxy_pid = proxy_start(1);
						continue;
					}
				}
			}
		}
#endif
		// TODO rewrite without using exit code (targeted at 0.9.7)


#ifdef __sun__
		/* horrible hack... what the FU*K is doing Solaris ??? */
		if (WIFSIGNALED(waitpid_status)) {
			if (uwsgi.to_heaven) {
				ready_to_reload++;
				continue;
			}
			else if (uwsgi.to_hell) {
				ready_to_die++;
				continue;
			}
		}
#endif
		/* check for reloading */
		if (WIFEXITED(waitpid_status)) {
			if (WEXITSTATUS(waitpid_status) == UWSGI_RELOAD_CODE && uwsgi.to_heaven) {
				ready_to_reload++;
				continue;
			}
			else if (WEXITSTATUS(waitpid_status) == UWSGI_END_CODE && uwsgi.to_hell) {
				ready_to_die++;
				continue;
			}
		}


		uwsgi.mywid = find_worker_id(diedpid);
		if (uwsgi.mywid <= 0) {
			if (WIFEXITED(waitpid_status)) {
				uwsgi_log("subprocess %d exited with code %d\n", (int) diedpid, WEXITSTATUS(waitpid_status));
			}
			else if (WIFSIGNALED(waitpid_status)) {
				uwsgi_log("subprocess %d exited by signal %d\n", (int) diedpid, WTERMSIG(waitpid_status));
			}
			else if (WIFSTOPPED(waitpid_status)) {
				uwsgi_log("subprocess %d stopped\n", (int) diedpid);
			}
			continue;
		}

		uwsgi_log( "DAMN ! process %d died :( trying respawn ...\n", diedpid);
		gettimeofday(&last_respawn, NULL);
		if (last_respawn.tv_sec == uwsgi.respawn_delta) {
			uwsgi_log( "worker respawning too fast !!! i have to sleep a bit...\n");
			/* TODO, user configurable fork throttler */
			sleep(2);
		}
		gettimeofday(&last_respawn, NULL);
		uwsgi.respawn_delta = last_respawn.tv_sec;
		// close the communication pipe
		/*
		close(uwsgi.workers[uwsgi.mywid].pipe[0]);
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, uwsgi.workers[uwsgi.mywid].pipe)) {
			uwsgi_error("socketpair()\n");
			continue;
		}
		*/
		pid = fork();
		if (pid == 0) {
			// fix the communication pipe
			close(uwsgi.shared->worker_signal_pipe[0]);
			uwsgi.mypid = getpid();
			uwsgi.workers[uwsgi.mywid].pid = uwsgi.mypid;
			uwsgi.workers[uwsgi.mywid].harakiri = 0;
			uwsgi.workers[uwsgi.mywid].requests = 0;
			uwsgi.workers[uwsgi.mywid].failed_requests = 0;
			uwsgi.workers[uwsgi.mywid].respawn_count++;
			uwsgi.workers[uwsgi.mywid].last_spawn = current_time;
			uwsgi.workers[uwsgi.mywid].manage_next_request = 1;
			break;
		}
		else if (pid < 1) {
			uwsgi_error("fork()");
		}
		else {
			uwsgi_log( "Respawned uWSGI worker (new pid: %d)\n", pid);
			//close(uwsgi.workers[uwsgi.mywid].pipe[1]);
			//event_queue_add_fd_read(uwsgi.master_queue, uwsgi.workers[uwsgi.mywid].pipe[0]);
#ifdef UWSGI_SPOOLER
			if (uwsgi.mywid <= 0 && diedpid != uwsgi.shared->spooler_pid) {
#else
				if (uwsgi.mywid <= 0) {
#endif

#ifdef UWSGI_PROXY
			// TODO if no gateway span the error !!!
			if (diedpid != uwsgi.shared->proxy_pid) {
#endif
						uwsgi_log( "warning the died pid was not in the workers list. Probably you hit a BUG of uWSGI\n");
#ifdef UWSGI_PROXY
					}
#endif
				}
			}
		}

	}
