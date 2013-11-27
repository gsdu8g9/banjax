/*
 * This class implmentats callbacks function that are called by ATS
 * AUTHORS:
 *   Vmon: May 2013, moving Bill's code to C++
 */

#include <stdio.h>
#include <ts/ts.h>
#include <regex.h>
#include <string.h>

#include <string>
#include <vector>
#include <list>

#include <zmq.hpp>
using namespace std;

#include <re2/re2.h>
//to retrieve the client ip
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

//to run fail2ban-client
#include <stdlib.h>

#include "banjax.h"
#include "banjax_continuation.h"
#include "transaction_muncher.h"
#include "regex_manager.h"
#include "challenge_manager.h"
#include "swabber_interface.h"
#include "ats_event_handler.h"

/*the glabal_cont the global continuation that is generated by
  by the banjax object.*/
//TSCont Banjax::global_contp;
//extern TSMutex Banjax::regex_mutex;
bool ATSEventHandler::banjax_active_queues[BanjaxFilter::TOTAL_NO_OF_QUEUES];

//Banjax* ATSEventHandler::banjax = NULL;

int
ATSEventHandler::banjax_global_eventhandler(TSCont contp, TSEvent event, void *edata)
{
  TSHttpTxn txnp = (TSHttpTxn) edata;
  BanjaxContinuation *cd;

  switch (event) {
  case TS_EVENT_HTTP_TXN_START:
    //If we are here it means this is the global continuation
    //we never subscribe subsequent continuations to this event
    handle_txn_start((TSHttpTxn) edata);
    /*if (banjax_active_queues[HTTP_START])
      handle_task_queue(HTTP_START, (BanjaxContinuation *) TSContDataGet(contp));*/

    return 0;

  case TS_EVENT_HTTP_READ_REQUEST_HDR:
    if(contp != Banjax::global_contp)
      handle_request((BanjaxContinuation *) TSContDataGet(contp));
      return 0;

  case TS_EVENT_HTTP_READ_CACHE_HDR:
    /* on hit we don't do anything for now
       lack of miss means hit to me 
       if (contp != Banjax::global_contp) {
       cd = (BanjaxContinuation *) TSContDataGet(contp);
       cd->hit = 1;
       }*/
    TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
    return 0;

  case TS_EVENT_HTTP_SEND_REQUEST_HDR:
    TSDebug(BANJAX_PLUGIN_NAME, "miss");
    if (contp != Banjax::global_contp) {
	    cd = (BanjaxContinuation *) TSContDataGet(contp);
	    cd->transaction_muncher.miss();
    }
    TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
    return 0;

  case TS_EVENT_HTTP_SEND_RESPONSE_HDR:
    //TSDebug(BANJAX_PLUGIN_NAME, "response" );
    if (contp != Banjax::global_contp) {
      cd = (BanjaxContinuation*) TSContDataGet(contp);
      handle_response(cd);
    }
    return 0;

  case TS_EVENT_HTTP_TXN_CLOSE:
    TSDebug(BANJAX_PLUGIN_NAME, "txn close" );
    if (contp != Banjax::global_contp) {
      cd = (BanjaxContinuation *) TSContDataGet(contp); 
      if (banjax_active_queues[BanjaxFilter::HTTP_CLOSE])
        handle_task_queue(banjax->task_queues[BanjaxFilter::HTTP_CLOSE], cd);

      //killing the continuation
      cd->~BanjaxContinuation(); //leave mem manage to ATS
      //TSfree(cd); I think TS is taking care of this
      destroy_continuation(contp);
    }
    break;

  case TS_EVENT_TIMEOUT:
    //TODO: This code does not make sense and needs to be revisited
    TSDebug("banjaxtimeout", "timeout" );
    /* when mutex lock is not acquired and continuation is rescheduled,
       the plugin is called back with TS_EVENT_TIMEOUT with a NULL
       edata. We need to decide, in which function did the MutexLock
       failed and call that function again */
    /*if (contp != Banjax::global_contp) {
      cd = (BanjaxContinuation *) TSContDataGet(contp);
      switch (cd->cf) {
        case BanjaxContinuation::HANDLE_REQUEST:
          handle_request(cd);
          return 0;
        default:
          TSDebug(BANJAX_PLUGIN_NAME, "This event was unexpected: %d\n", event);
          break;
	  }
    } else {
      //regardless, it even doesn't make sense to read the list here
      //read_regex_list(contp);
      return 0;
      }*/

    default:
      TSDebug(BANJAX_PLUGIN_NAME, "Unsolicitated event call?" );
      break;
  }

  return 0;

}

void
ATSEventHandler::handle_request(BanjaxContinuation* cd)
{
  //retreiving part of header requested by the filters
  const TransactionParts& cur_trans_parts = cd->transaction_muncher.retrieve_parts(banjax->which_parts_are_requested());

  bool continue_filtering = true;
  for(Banjax::TaskQueue::iterator cur_task = banjax->task_queues[BanjaxFilter::HTTP_REQUEST].begin(); continue_filtering && cur_task != banjax->task_queues[BanjaxFilter::HTTP_REQUEST].end(); cur_task++) {
    FilterResponse cur_filter_result = ((*(cur_task->filter)).*(cur_task->task))(cur_trans_parts);
    switch (cur_filter_result.response_type) 
      {
      case FilterResponse::GO_AHEAD_NO_COMMENT:
        continue;
        
      case FilterResponse::NO_WORRIES_SERVE_IMMIDIATELY: 
        //This is when the requester is white listed
        continue_filtering = false;
        break;

      case FilterResponse::I_RESPOND:
        // from here on, cur_filter_result is owned by the continuation data.
        cd->response_info = cur_filter_result;
        cd->responding_filter = cur_task->filter;
        // TODO(oschaaf): commented this. @vmon: we already hook this globally,
        // is there a reason we need to hook it again here?
        //TSHttpTxnHookAdd(cd->txnp, TS_HTTP_SEND_RESPONSE_HDR_HOOK, cd->contp);
        TSHttpTxnReenable(cd->txnp, TS_EVENT_HTTP_ERROR);
        return;

      default:
        //Not implemeneted, hence ignoe
        break;
        
      }
  }
  
  //TODO: it is imaginable that a filter needs to be 
  //called during response but does not need to influnence
  //the response, e.g. botbanger hence we need to get the 
  //response hook while continuing with the flow
  //destroy_continuation(cd->txnp, cd->contp);
  TSHttpTxnReenable(cd->txnp, TS_EVENT_HTTP_CONTINUE);

}

void
ATSEventHandler::handle_response(BanjaxContinuation* cd)
{
  //we need to retrieve response parts for any filter who requested it.
  cd->transaction_muncher.retrieve_response_parts(banjax->which_response_parts_are_requested());

  if (cd->response_info.response_type == FilterResponse::I_RESPOND) {
    cd->transaction_muncher.set_status(TS_HTTP_STATUS_FORBIDDEN);
    std::string buf = (((cd->responding_filter)->*(((FilterExtendedResponse*)cd->response_info.response_data)->response_generator)))(cd->transaction_muncher.retrieve_parts(banjax->all_filters_requested_part), cd->response_info);

    cd->transaction_muncher.set_status(
        (TSHttpStatus)(((FilterExtendedResponse*)cd->response_info.response_data))->response_code);
    
    if ((((FilterExtendedResponse*)cd->response_info.response_data))->set_cookie_header.size()) {
      cd->transaction_muncher.append_header(
          "Set-Cookie", (((FilterExtendedResponse*)cd->response_info.response_data))->set_cookie_header.c_str());
    }
    if (buf.size() == 0) {
      // When we get here, no valid response body was generated somehow.
      // Insert one, to prevent triggering an assert in TSHttpTxnErrorBodySet
      buf.append("Not authorized");
    }
    char* b = (char*) TSmalloc(buf.size());
    memcpy(b, buf.data(), buf.size());
    TSHttpTxnErrorBodySet(cd->txnp, b, buf.size(),
                          (((FilterExtendedResponse*)cd->response_info.response_data))->get_and_release_content_type());
  }
  //Now we should take care of registerd filters in the queue these are not
  //going to generate the response at least that is the plan
  if (banjax_active_queues[BanjaxFilter::HTTP_START])
    handle_task_queue(banjax->task_queues[BanjaxFilter::HTTP_RESPONSE], cd);

  TSHttpTxnReenable(cd->txnp, TS_EVENT_HTTP_CONTINUE);

}

/**
   @param global_contp contains the global continuation and is sent here
   , so the new continuation gets the main banjax object
 */
void
ATSEventHandler::handle_txn_start(TSHttpTxn txnp)
{
  TSDebug(BANJAX_PLUGIN_NAME, "txn start" );

  TSCont txn_contp;
  BanjaxContinuation *cd;

  //retreive the banjax obej
  txn_contp = TSContCreate((TSEventFunc) banjax_global_eventhandler, TSMutexCreate());
  /* create the data that'll be associated with the continuation */
  cd = (BanjaxContinuation *) TSmalloc(sizeof(BanjaxContinuation));
  cd = new(cd) BanjaxContinuation(txnp);
  //TSDebug(BANJAX_PLUGIN_NAME, "New continuation data at %lu", (unsigned long)cd);
  TSContDataSet(txn_contp, cd);

  cd->contp = txn_contp;

  TSHttpTxnHookAdd(txnp, TS_HTTP_READ_REQUEST_HDR_HOOK, txn_contp);
  TSHttpTxnHookAdd(txnp, TS_HTTP_SEND_REQUEST_HDR_HOOK, txn_contp);
  TSHttpTxnHookAdd(txnp, TS_HTTP_SEND_RESPONSE_HDR_HOOK, txn_contp);
  TSHttpTxnHookAdd(txnp, TS_HTTP_TXN_CLOSE_HOOK, txn_contp);

  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);

}

/**
   runs if a filter is registered to run a function during an event
*/
void 
ATSEventHandler::handle_task_queue(Banjax::TaskQueue& current_queue, BanjaxContinuation* cd)
{
  const TransactionParts& cur_trans_parts = cd->transaction_muncher.retrieve_parts(banjax->which_parts_are_requested());

  for(Banjax::TaskQueue::iterator cur_task = current_queue.begin(); cur_task != current_queue.end(); cur_task++)
    /*For now we have no plan on doing anything with the result
      in future we need to receive the filter instruction for 
      the rest of transation but for the moment only it matters
      at the begining of the transaction to interfere with the 
      transaction.

      FilterResponse cur_filter_result =
     */ 
    ((*(cur_task->filter)).*(cur_task->task))(cur_trans_parts);
  
}

void
ATSEventHandler::destroy_continuation(TSCont contp)
{
  BanjaxContinuation *cd = NULL;

  cd = (BanjaxContinuation *) TSContDataGet(contp);

  //save the txn before destroying the continuation so we can continue
  TSHttpTxn txn_keeper = cd->txnp;
  TSfree(cd);

  TSContDestroy(contp);
  TSHttpTxnReenable(txn_keeper, TS_EVENT_HTTP_CONTINUE);

}
