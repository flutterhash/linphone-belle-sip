/*
	belle-sip - SIP (RFC3261) library.
    Copyright (C) 2010  Belledonne Communications SARL

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "belle_sip_internal.h"
#include "sender_task.h"

struct belle_sip_transaction{
	belle_sip_object_t base;
	belle_sip_provider_t *provider; /*the provider that created this transaction */
	belle_sip_request_t *request;
	belle_sip_response_t *prov_response;
	belle_sip_response_t *final_response;
	char *branch_id;
	belle_sip_transaction_state_t state;
	belle_sip_sender_task_t *stask;
	uint64_t start_time;
	int is_reliable;
	void *appdata;
};


static belle_sip_source_t * transaction_create_timer(belle_sip_transaction_t *t, belle_sip_source_func_t func, unsigned int time_ms){
	belle_sip_stack_t *stack=belle_sip_provider_get_sip_stack(t->provider);
	belle_sip_source_t *s=belle_sip_timeout_source_new (func,t,time_ms);
	belle_sip_main_loop_add_source(stack->ml,s);
	return s;
}

/*
static void transaction_remove_timeout(belle_sip_transaction_t *t, unsigned long id){
	belle_sip_stack_t *stack=belle_sip_provider_get_sip_stack(t->provider);
	belle_sip_main_loop_cancel_source (stack->ml,id);
}
*/

static void belle_sip_transaction_init(belle_sip_transaction_t *t, belle_sip_provider_t *prov, belle_sip_request_t *req){
	belle_sip_object_init_type(t,belle_sip_transaction_t);
	if (req) belle_sip_object_ref(req);
	t->request=req;
	t->provider=prov;
}

static void transaction_destroy(belle_sip_transaction_t *t){
	if (t->request) belle_sip_object_unref(t->request);
	if (t->prov_response) belle_sip_object_unref(t->prov_response);
	if (t->final_response) belle_sip_object_unref(t->final_response);
	if (t->stask) belle_sip_object_unref(t->stask);
}

void *belle_sip_transaction_get_application_data(const belle_sip_transaction_t *t){
	return t->appdata;
}

void belle_sip_transaction_set_application_data(belle_sip_transaction_t *t, void *data){
	t->appdata=data;
}

const char *belle_sip_transaction_get_branch_id(const belle_sip_transaction_t *t){
	return t->branch_id;
}

belle_sip_transaction_state_t belle_sip_transaction_get_state(const belle_sip_transaction_t *t){
	return t->state;
}

void belle_sip_transaction_terminate(belle_sip_transaction_t *t){
	
}

belle_sip_request_t *belle_sip_transaction_get_request(belle_sip_transaction_t *t){
	return t->request;
}

/*
 Server transaction
*/

struct belle_sip_server_transaction{
	belle_sip_transaction_t base;
};

void belle_sip_server_transaction_send_response(belle_sip_server_transaction_t *t){
}

static void server_transaction_destroy(belle_sip_server_transaction_t *t){
	transaction_destroy((belle_sip_transaction_t*)t);
}

belle_sip_server_transaction_t * belle_sip_server_transaction_new(belle_sip_provider_t *prov,belle_sip_request_t *req){
	belle_sip_server_transaction_t *t=belle_sip_object_new(belle_sip_server_transaction_t,(belle_sip_object_destroy_t)server_transaction_destroy);
	belle_sip_transaction_init((belle_sip_transaction_t*)t,prov,req);
	return t;
}

/*
 Client transaction
*/

struct belle_sip_client_transaction{
	belle_sip_transaction_t base;
	belle_sip_source_t *timer;
	int interval;
	uint64_t timer_F;
	uint64_t timer_E;
	uint64_t timer_K;
};

belle_sip_request_t * belle_sip_client_transaction_create_cancel(belle_sip_client_transaction_t *t){
	return NULL;
}

static int on_client_transaction_timer(void *data, unsigned int revents){
	belle_sip_client_transaction_t *t=(belle_sip_client_transaction_t*)data;
	const belle_sip_timer_config_t *tc=belle_sip_stack_get_timer_config (belle_sip_provider_get_sip_stack (t->base.provider));
	
	if (t->base.state==BELLE_SIP_TRANSACTION_TRYING){
		belle_sip_sender_task_send(t->base.stask);
		t->interval=MIN(t->interval*2,tc->T2);
		belle_sip_source_set_timeout(t->timer,t->interval);
	}
	
	if (belle_sip_time_ms()>=t->timer_F){
		belle_sip_transaction_terminate((belle_sip_transaction_t*)t);
		return BELLE_SIP_STOP;
	}
	return BELLE_SIP_CONTINUE;
}

static void client_transaction_cb(belle_sip_sender_task_t *task, void *data, int retcode){
	belle_sip_client_transaction_t *t=(belle_sip_client_transaction_t*)data;
	const belle_sip_timer_config_t *tc=belle_sip_stack_get_timer_config (belle_sip_provider_get_sip_stack (t->base.provider));
	if (retcode==0){
		t->base.is_reliable=belle_sip_sender_task_is_reliable(task);
		t->base.state=BELLE_SIP_TRANSACTION_TRYING;
		t->base.start_time=belle_sip_time_ms();
		t->timer_F=t->base.start_time+(tc->T1*64);
		if (!t->base.is_reliable){
			t->interval=tc->T1;
			t->timer=transaction_create_timer(&t->base,on_client_transaction_timer,tc->T1);
		}else{
			t->timer=transaction_create_timer(&t->base,on_client_transaction_timer,tc->T1*64);
		}
	}else{
		belle_sip_transaction_terminated_event_t ev;
		ev.source=t->base.provider;
		ev.transaction=(belle_sip_transaction_t*)t;
		ev.is_server_transaction=FALSE;
		BELLE_SIP_PROVIDER_INVOKE_LISTENERS(t->base.provider,process_transaction_terminated,&ev);
	}
}


void belle_sip_client_transaction_send_request(belle_sip_client_transaction_t *t){
	t->base.stask=belle_sip_sender_task_new(t->base.provider,BELLE_SIP_MESSAGE(t->base.request),client_transaction_cb,t);
	belle_sip_sender_task_send(t->base.stask);
}

/*called by the transport layer when a response is received */
void belle_sip_client_transaction_add_response(belle_sip_client_transaction_t *t, belle_sip_response_t *resp){
	int code=belle_sip_response_get_status_code(resp);
	
	if (code>=100 && code<200){
		switch(t->base.state){
			case BELLE_SIP_TRANSACTION_TRYING:
			case BELLE_SIP_TRANSACTION_PROCEEDING:
				t->base.state=BELLE_SIP_TRANSACTION_PROCEEDING;
				if (t->base.prov_response!=NULL){
					belle_sip_object_unref(t->base.prov_response);
				}
				t->base.prov_response=(belle_sip_response_t*)belle_sip_object_ref(resp);
			break;
			default:
				belle_sip_warning("Unexpected provisional response while transaction in state %i",t->base.state);
		}
	}else if (code>=200){
		switch(t->base.state){
			case BELLE_SIP_TRANSACTION_TRYING:
			case BELLE_SIP_TRANSACTION_PROCEEDING:
				t->base.state=BELLE_SIP_TRANSACTION_COMPLETED;
				t->base.final_response=(belle_sip_response_t*)belle_sip_object_ref(resp);
			break;
			default:
				belle_sip_warning("Unexpected final response while transaction in state %i",t->base.state);
		}
	}
}

static void client_transaction_destroy(belle_sip_client_transaction_t *t ){
	transaction_destroy((belle_sip_transaction_t*)t);
}


belle_sip_client_transaction_t * belle_sip_client_transaction_new(belle_sip_provider_t *prov, belle_sip_request_t *req){
	belle_sip_client_transaction_t *t=belle_sip_object_new(belle_sip_client_transaction_t,(belle_sip_object_destroy_t)client_transaction_destroy);
	belle_sip_transaction_init((belle_sip_transaction_t*)t,prov,req);
	return t;
}



