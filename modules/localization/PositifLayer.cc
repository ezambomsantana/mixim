/***************************************************************************
 * file:        PositifLayer.cc
 *
 * author:      Peterpaul Klein Haneveld
 *
 * copyright:   (C) 2007 Parallel and Distributed Systems Group (PDS) at
 *              Technische Universiteit Delft, The Netherlands.
 *
 *              This program is free software; you can redistribute it 
 *              and/or modify it under the terms of the GNU General Public 
 *              License as published by the Free Software Foundation; either
 *              version 2 of the License, or (at your option) any later 
 *              version.
 *              For further information see file COPYING 
 *              in the top level directory
 ***************************************************************************
 * part of:     mixim framework
 * description: localization layer: general class for the network layer
 *              subclass to create your own localization layer
 ***************************************************************************/


#include "PositifLayer.h"
#include "BaseUtility.h"
#include "FindModule.h"
#include "ModuleAccess.h"
#include "ConnectionManager.h"
#include "NetwControlInfo.h"
#include <SimpleAddress.h>
#include "winsupport.h"
#include "sorted_list.h"

Define_Module(PositifLayer);

int PositifLayer::num_nodes;
int PositifLayer::num_anchors;
int PositifLayer::algorithm;
int PositifLayer::version;
int PositifLayer::nr_dims;
node_info *PositifLayer::node = NULL;
FLOAT PositifLayer::area;
double *PositifLayer::dim;
FLOAT PositifLayer::range;
double PositifLayer::range_variance;
double PositifLayer::pos_variance;
char PositifLayer::topology_type[TOPOLOGYTYPE_LENGTH];
int PositifLayer::flood_limit;
int PositifLayer::refine_limit;
int PositifLayer::tri_alg;
bool PositifLayer::do_2nd_phase = true;
int PositifLayer::phase1_min_anchors;
int PositifLayer::phase1_max_anchors;
FLOAT PositifLayer::var0;
FLOAT PositifLayer::var1;
FLOAT PositifLayer::var2;
BaseWorldUtility * PositifLayer::world;
glob_info * PositifLayer::ginfo;
FILE * PositifLayer::scenario;

struct myParams params;

timer_info _timers[MAX_TIMERS];

/*******************************************************************************
 * static functions
 ******************************************************************************/
const static char *status2string(int status)
{
	const static char *anchor = "ANCHOR";
	const static char *unknown = "UNKNOWN";
	const static char *positioned = "POSITIONED";
	const static char *bad = "BAD";

	if (status == STATUS_ANCHOR)
		return anchor;
	else if (status == STATUS_UNKNOWN)
		return unknown;
	else if (status == STATUS_POSITIONED)
		return positioned;
	else
		return bad;
}

static void delete_cMessage(void *_msg)
{
	cMessage *msg = (cMessage *) _msg;
	delete msg;
}

static double square(double x)
{
	return x * x;
}

/*******************************************************************************
 * Initialization methods
 ******************************************************************************/
void PositifLayer::initialize(int stage)
{
	BaseLayer::initialize(stage);

	switch (stage) {
	case 0:
		RepeatTimer::init(this);
//              me = par("me");
		me = findHost()->index();
		/* clear arrays */
		for (int i = 0; i < MAX_MSG_TYPES; i++) {
			seqno[i] = last_sent_seqno[i] = 0;
		}

		/* only one node needs to initialize this */
		if (me == 0) {
			world = (BaseWorldUtility*)getGlobalModule("BaseWorldUtility");
			if (world == NULL) {
				error("Could not find BaseWorldUtility module");
			}
			setup_global_vars();
			node = new node_info[num_nodes];
		}
		break;
	case 1:
		{
			headerLength = par("headerLength");

			node[me].ID = me;
			node[me].anchor = par("anchor");
			node[me].recv_cnt = 0;
			/* initialize the node specific stuff */
			node[me].neighbors = new cLinkedList();
			node[me].perf_data.bcast_total = new int[MAX_MSG_TYPES];
			node[me].perf_data.bcast_unique =
			    new int[MAX_MSG_TYPES];
			for (int m = 0; m < nr_dims; m++)
				node[me].perf_data.curr_pos[m] = 0.0;
			node[me].perf_data.flops = 0;
			node[me].perf_data.confidence = 0.0;
			node[me].perf_data.err = -1.0;
			node[me].perf_data.phase1_err = -1.0;
			node[me].perf_data.phase2_err = -1.0;
			node[me].perf_data.status = STATUS_UNKNOWN;
			node[me].perf_data.anchor_range_error = 0.0;
			node[me].perf_data.rel_anchor_range_error = 0.0;
			node[me].perf_data.abs_anchor_range_error = 0.0;
			node[me].perf_data.abs_rel_anchor_range_error = 0.0;
			node[me].perf_data.anchor_range = 0.0;
			node[me].perf_data.anchor_range_error_count = 0;
			for (int m = 0; m < MAX_MSG_TYPES; m++) {
				node[me].perf_data.bcast_total[m] = 0;
				node[me].perf_data.bcast_unique[m] = 0;
			}

			refine_count = 0;

			log = node[me].perf_data.log;
			log[0] = '\0';
			reps = par("repeats");
			use_log = par("use_log");
			msg_buffering = par("msg_buffering");

			// Range errors
			used_anchors = 0;
			range_list = NULL;
			real_range_list = NULL;

			Coord coord = getPosition();
			/* convert from Coord to Position */
			node[me].true_pos[0] = position[0] = coord.getX();
			node[me].true_pos[1] = position[1] = coord.getY();
			node[me].true_pos[2] = position[2] = coord.getZ();

#ifndef NDEBUG
			EV << "node " << node[me].
			    ID << "'s true location: " <<
			    pos2str(node[me].true_pos) << "\n";
#endif

			/* In positif this is done by an offset:
			 * genk_normal(1, node[me].true_pos[i], range * pos_variance)
			 */
			if (node[me].anchor) {
				for (int i = 0; i < 3; i++)
					node[me].init_pos[i] = node[me].true_pos[i];
			} else {
				for (int i = 0; i < 3; i++)
// 					node[me].init_pos[i] =
// 						genk_normal(1, node[me].true_pos[i],
// 							    range * pos_variance);
					node[me].init_pos[i] = 0;
			}
		}
		break;
	case 2:
		{
			setup_grid();
			/* init must reserve the number of used repeat timers for the application */
			init();
			/* create the start timer, one repeat */
			start_timer = setRepeatTimer(1, 1);
			cMessage *msg = new cMessage("START", MSG_START);
			msg->addPar("anchor") = node[me].anchor;
			if (node[me].anchor) {
				add_struct(msg, "position", node[me].true_pos);
			} else {
				add_struct(msg, "position", node[me].init_pos);
			}
			setContextPointer(start_timer, msg);
			setContextDestructor(start_timer, delete_cMessage);
		}
		break;
	default:
		break;
	}
}

void PositifLayer::setup_global_vars(void)
{
	/*random_seed = (long *) malloc(NUM_RAND_GEN * sizeof(long));

	   for (int i = 0; i < NUM_RAND_GEN; i++)
	   random_seed[i] = genk_randseed(i);   // Save for output purposes. */

	node = NULL;
	nr_dims = (world->use2D()?2:3);

	dim = new double[nr_dims];

	switch (nr_dims) {
	case 3: dim[2] = world->getPgs()->getZ();
	case 2: dim[1] = world->getPgs()->getY();
	case 1: dim[0] = world->getPgs()->getX();
		break;
	default:
		error("initialize() can't handle %d-dimensional space", nr_dims);
		abort();
	}

	area = dim[0];
	for (int d = 1; d < nr_dims; d++) {
		area *= dim[d];
	}

//      num_nodes = par("num_nodes");
	num_nodes = simulation.systemModule()->par("numHosts");
	fprintf(stdout, "numHosts = %d\n", num_nodes);
	/** @TODO This might need to be determined based on the actual number of nodes. */
	num_anchors =
	    (int) Max(num_nodes * (double) par("anchor_frac"), nr_dims + 1);
//	range = (double) par("range");
	range = (double)getGlobalModule("channelcontrol")->par("radioRange");
	do_2nd_phase = par("do_2nd_phase");	// Each node writes to these global variables, but this shouldn't cause any problems.

	phase1_min_anchors = par("phase1_min_anchors");
	phase1_max_anchors = par("phase1_max_anchors");
	flood_limit = par("flood_limit");
	refine_limit = par("refine_limit");
	tri_alg = par("tri_alg");
	var0 = (double) par("var0");
	var1 = (double) par("var1");
	var2 = (double) par("var2");
}

void PositifLayer::setup_grid()
{
	if (me == 0) {
		//      double grid_d = par("grid_d");

		range_variance = par("range_variance");
		snprintf(topology_type, TOPOLOGYTYPE_LENGTH, "Grid");
		pos_variance = par("pos_variance");

		// Select random anchors
// 		for (int i = 0; i < num_anchors; i++) {
// 			int n;

			// Initialize random anchors
// 			do {
// 				n = (int) genk_uniform(1, 0, num_nodes - 1);
// 			} while (node[n].anchor);
// 			node[n].anchor = true;

// 			memcpy(node[n].init_pos, node[n].true_pos,
// 			       sizeof(node[n].true_pos));
// 		}
	}
}

/*******************************************************************************
 * Finalization methods
 ******************************************************************************/
void PositifLayer::finish()
{
	handleStopMessage(NULL);
	{
		static int count = 0;
		count++;
		if (count == num_nodes) {
			write_statistics();
			write_configuration("configuration.scen");
		}
	}
	delete node[me].neighbors;
}

/*******************************************************************************
 * Localization methods
 ******************************************************************************/
Coord PositifLayer::getPosition()
{
	BaseUtility *util =
	    FindModule < BaseUtility * >::findSubModule(findHost());
	const Coord *coord = util->getPos();
	return *coord;
}

/* Check if this node exists in neighbor list. */
bool PositifLayer::isNewNeighbor(int src)
{
	for (cLinkedListIterator iter =
	     cLinkedListIterator(*node[me].neighbors); !iter.end(); iter++) {
		neighbor_info *n = (neighbor_info *) iter();
		if (n->idx == src)
			return false;
	}
	return true;
}

void PositifLayer::addNewNeighbor(int src, double distance,
				   double estimated_distance)
{
	neighbor_info *n = new neighbor_info;
	n->idx = src;
	n->true_dist = distance;
	// Use estimated distance here
	if (estimated_distance > range)
		estimated_distance = range;
	n->est_dist = estimated_distance;
	node[me].neighbors->insert(n);
}

int PositifLayer::logprintf(__const char *__restrict __format, ...)
{
	if (!use_log)
		return 0;

	assert(log != NULL);

	va_list pars;
	va_start(pars, __format);
	int log_len = strlen(log);
	int res = vsnprintf(log + log_len, LOGLENGTH - log_len, __format, pars);
	va_end(pars);

	assert(res > 0);
	return res;
}

FLOAT PositifLayer::distance(Position a, Position b)
{
	FLOAT sumsqr = 0;
	for (int i = 0; i < nr_dims; i++) {
		sumsqr += (a[i] - b[i]) * (a[i] - b[i]);
	}
	return sqrt(sumsqr);
}

char *PositifLayer::pos2str(Position a)
{
	static char str[100];

	switch (nr_dims) {
	case 2:
		sprintf(str, "<%.2f,%.2f>", a[0], a[1]);
		break;
	case 3:
		sprintf(str, "<%.2f,%.2f,%.2f>", a[0], a[1], a[2]);
		break;
	default:
		abort();
	}
	return str;
}

void PositifLayer::update_perf_data()
{
	memcpy(node[me].perf_data.curr_pos, position, sizeof(position));
	node[me].perf_data.flops = flops;
	node[me].perf_data.confidence = confidence;
	node[me].perf_data.status = status;
	node[me].perf_data.err = distance(position, node[me].true_pos) / range;
	node[me].perf_data.used_anchors = used_anchors;
	node[me].perf_data.range_list = range_list;
	node[me].perf_data.real_range_list = real_range_list;
}

/*******************************************************************************
 * MiXiM layering methods
 ******************************************************************************/
/**
 * Decapsulates the packet from the received Network packet 
 **/
cMessage *PositifLayer::decapsMsg(LocPkt * msg)
{
	cMessage *m = msg->decapsulate();

	cPolymorphic *cInfo = msg->removeControlInfo();
	if (cInfo != NULL) {
		m->setControlInfo(cInfo);
	}

	EV << " pkt decapsulated\n";

	return m;
}

/**
 * Encapsulates the received ApplPkt into a LocPkt and set all needed
 * header fields.
 **/
LocPkt *PositifLayer::encapsMsg(cMessage * msg)
{
	LocPkt *pkt = new LocPkt(msg->name(), msg->kind());
	pkt->setLength(headerLength);

	cPolymorphic *cInfo = msg->removeControlInfo();
	if (cInfo != NULL) {
		pkt->setControlInfo(cInfo);
	}
	//encapsulate the application packet
	pkt->encapsulate(msg);

	EV << " pkt encapsulated\n";
	return pkt;
}

/**
 * Redefine this function if you want to process messages from lower
 * layers before they are forwarded to upper layers
 *
 *
 * If you want to forward the message to upper layers please use
 * @ref sendUp which will take care of decapsulation and thelike
 **/
void PositifLayer::handleLowerMsg(cMessage * msg)
{
	LocPkt *m = dynamic_cast < LocPkt * >(msg);

	/* Get the distance between the source and this node.
	 * This is cheated, and should be calculated based on ultrasone
	 * or RSSI. Currently the actual distance between the nodes is
	 * used. */
	int src = msg->par("src");
	double distance =
	    sqrt(square(node[me].true_pos[0] - node[src].true_pos[0]) +
		 square(node[me].true_pos[1] - node[src].true_pos[1]) +
		 square(node[me].true_pos[2] - node[src].true_pos[2]));
	msg->addPar("distance") = distance;

//      fprintf (stderr, "[%d] PositifLayer::handleLowerMsg() from %d\n",
//               me, src);

	node[me].recv_cnt++;
	/* If we received a LocPkt, this was received from
	 * upper layers. */
	if (m) {
		if (isNewNeighbor(src))
			addNewNeighbor(src, distance, distance);
		sendUp(decapsMsg(m));
	} else {
		switch (msg->kind()) {
		case MSG_NEIGHBOR:
//                      fprintf(stderr, "[%d] Received MSG_NEIGHBOR from [%d]\n", me, src);
			assert(isNewNeighbor(src));
			addNewNeighbor(src, distance, distance);
			break;
		default:
			{
				bool behind = !msg_buffering
				    && !putAside.empty();
				if (!behind) {
					bool new_neighbor =
					    isNewNeighbor(src);
					if (new_neighbor)
						addNewNeighbor(src, distance,
								distance);

					handleMessage(msg, new_neighbor);
//                                      // filter out redundant msg
//                                      int s = msg->par("seqno");
//                                      int kind = msg->kind();
//                                      assert(s >= neighbor->seqno[kind]);
//                                      // A message is new if it has a higher sequence number
//                                      if (s > neighbor->seqno[kind]) {
//                                              neighbor->seqno[kind] = s;
//                                              handleMessage(msg, new_neighbor);
//                                      }
				}
			}
		}
	}
	update_perf_data();
	delete msg;
}

/**
 * Redefine this function if you want to process messages from upper
 * layers before they are send to lower layers.
 *
 * For the PositifLayer we just use the destAddr of the network
 * message as a nextHop
 *
 * To forward the message to lower layers after processing it please
 * use @ref sendDown. It will take care of anything needed
 **/
void PositifLayer::handleUpperMsg(cMessage * msg)
{
	sendDown(encapsMsg(msg));
}

/**
 * Redefine this function if you want to process control messages
 * from lower layers. 
 *
 * This function currently handles one messagetype: TRANSMISSION_OVER.
 * If such a message is received in the network layer it is deleted.
 * This is done as this type of messages is passed on by the BaseMacLayer.
 *
 * It may be used by network protocols to determine when the lower layers
 * are finished sending a message.
 **/
void PositifLayer::handleLowerControl(cMessage * msg)
{
	switch (msg->kind()) {
	default:
		opp_warning
		    ("PositifLayer does not handle control messages called %s",
		     msg->name());
		delete msg;
	}
}

// Functions that subclasses should implement.
// (They can't be abstract because of OMNET++
void PositifLayer::handleStopMessage(cMessage * msg)
{
	if (status == STATUS_POSITIONED) {
		fprintf(stderr, "Used anchors for node %d\n", me);
		for (int i = 0; i < used_anchors; i++)
			fprintf(stderr,
				"used_anchor: range %4.2f real range %4.2f error %4.2f rel error %4.2f\n",
				range_list[i], real_range_list[i],
				range_list[i] - real_range_list[i],
				(range_list[i] -
				 real_range_list[i]) / real_range_list[i]);
	}
}

/*******************************************************************************
 * Timer methods
 ******************************************************************************/
// timer_info *PositifLayer::timer(int reps, int handler, void *arg)
// {
//      timer_info *e = new timer_info;

//      e->repeats = e->cnt = reps;
//      e->handler = handler;
//      e->arg = arg;

//      return e;
// }

timer_info *PositifLayer::timer(int reps, int handler, void *arg)
{
	assert(handler < MAX_TIMERS);
	setRepeatTimer(handler, 1, reps);
	if (arg)
		setContextPointer(handler, arg);
	_timers[handler] = handler;
	return &_timers[handler];
}

// void PositifLayer::resetTimer(timer_info * e)
// {
//      if (e)
//              e->cnt = e->repeats;
// }

void PositifLayer::resetTimer(timer_info * e)
{
	if (e)
		resetRepeatTimer(*e);
}

// void PositifLayer::invokeTimer(timer_info * e)
// {
//      if (e) {
//              if (e->cnt > 0) {
//                      e->cnt--;
//                      handleTimer(e);
//              }
//      }
// }

void PositifLayer::cancelTimer(timer_info * e)
{
	cancelRepeatTimer(*e);
}

// addTimer isn't needed anymore, since timer() already adds it.
// void PositifLayer::addTimer(timer_info * e)
// {
//      if (e)
//              timeouts.insert(e);
// }

void PositifLayer::addTimer(timer_info * e)
{
	assert(e);
	assert(*e >= 0 && *e < MAX_TIMERS);
}

// iterator has to be done differently
// cLinkedListIterator PositifLayer::getTimers(void)
// {
//      return cLinkedListIterator(timeouts);
// }

/* Normally all repeatTimers are handled by the algorithm, but there's
 * one special message -start_timer- that is needed to initialize the
 * network. Therefor all localization modules must call:
 * - PositifLayer::handleRepeatTimer on the default case. */
void PositifLayer::handleRepeatTimer(unsigned int index)
{
	Enter_Method_Silent();

	if (index == UINT_MAX) {
		error("Uninitialized timer called? aborting now...");
		abort();
	} else if (index == start_timer) {
//              fprintf(stderr, "[%d] received: %s\n", me, "START_TIMER");
		cMessage *msg = (cMessage *) contextPointer(index);
		assert(msg->kind() == MSG_START);
		status = msg->par("anchor") ? STATUS_ANCHOR : STATUS_UNKNOWN;
		// Anchors start with their position set
		if (status == STATUS_ANCHOR) {
			get_struct(msg, "position", position);
			confidence = 1.0;
		} else {
			for (int i = 0; i < nr_dims; i++)
				position[i] = 0.0;
			confidence = 0.0;
		}

		flops = 0;

#ifndef NDEBUG
		EV << node[me].
		    ID << ": starts at pos " << pos2str(position) << '\n';
#endif

		handleStartMessage(msg);
		deleteRepeatTimer(index);
		start_timer = UINT_MAX;

		cMessage *neighborMsg =
		    new cMessage("NEIGHBOR", MSG_NEIGHBOR);
		send(neighborMsg);
	} else {
		assert(index < MAX_TIMERS);
		/* analyzeTopology should be executed the first time */
		{
			static bool done = false;
			if (!done) {
				analyzeTopology();
				statistics(true);
			}
			done = true;
		}
		handleTimer(&_timers[index]);
	}
}

/*******************************************************************************
 * Communication methods
 ******************************************************************************/
void PositifLayer::send(cMessage * msg)	// Synchronous send
{
	int kind = msg->kind();

	// Add header
	msg->addPar("src") = me;
	msg->addPar("seqno") = seqno[kind];
	msg->setControlInfo(new NetwControlInfo(L3BROADCAST));

#ifndef NDEBUG
	EV << node[me].ID << ": broadcast " << msg->name();
//      if (kind == MSG_POSITION) {
//              EV << " confidence = " << msg->par("confidence");
//      }
	EV << " seqno = " << seqno[kind] << "\n";
#endif

	if (kind >= MSG_TYPE_BASE) {
		node[me].perf_data.bcast_total[kind]++;
		if (last_sent_seqno[kind] < seqno[kind]) {
			last_sent_seqno[kind] = seqno[kind];
			node[me].perf_data.bcast_unique[kind]++;
		}
	}
	sendDown(msg);

	return;
}

/*******************************************************************************
 * Triangulate methods
 ******************************************************************************/
FLOAT PositifLayer::savvides_minmax(int n_pts, FLOAT ** positions,
				    FLOAT * ranges, FLOAT * confs, int target)
{
	//
	//  Savvides' algorithm
	//
	Position min, max;

	// Find the max-min and min-max in each dimension.
	for (int i = 0; i < n_pts; i++)
		for (int j = 0; j < nr_dims; j++) {
			if (positions[i][j] - ranges[i] > min[j] || i == 0)
				min[j] = positions[i][j] - ranges[i];
			if (positions[i][j] + ranges[i] < max[j] || i == 0)
				max[j] = positions[i][j] + ranges[i];
		}

	// Store the result (avg of min and max)
	for (int i = 0; i < nr_dims; i++) {
		positions[n_pts][i] = (min[i] + max[i]) / 2;
	}

	FLOAT residu = 0;
	FLOAT sum_c = 0;
	for (int j = 0; j < n_pts; j++) {
		FLOAT c = (confs == NULL ? 1 : confs[j]);
		residu +=
		    c * fabs(ranges[j] -
			     distance(positions[n_pts], positions[j]));
		sum_c += c;
	}
	residu /= sum_c;

	return residu;
}

FLOAT PositifLayer::triangulate(int n_pts, FLOAT ** positions,
				FLOAT * ranges, FLOAT * confs, int target)
{
	FLOAT dop;

	if (n_pts <= nr_dims)
		return -1;

#ifndef NDEBUG
	if (confs != NULL) {
		EV << "confs:";
		for (int j = 0; j < n_pts; j++) {
			EV << " " << confs[j];
		}
		EV << "\n";
	}
#endif

	::params.dim = nr_dims;
	::params.alg_sel = 0;
	::params.conf_mets = par("use_confs") && (confs != NULL);
	if (!::triangulate(&::params, n_pts, positions, ranges, confs,
			   positions[n_pts], &dop)) {
#ifndef NDEBUG
		EV << ": FAILED TRIANGULATION\n";
#endif
		return -1;
	}
	// triangulate() used the last point to linearize the equations.
	// Use the estimated position for an extra equation so all inputs are
	// treated equally.
	ranges[n_pts] = 0;
	::triangulate(&::params, n_pts + 1, positions, ranges, confs,
		      positions[n_pts], &dop);
#ifndef NDEBUG
	EV << ": " << pos2str(positions[n_pts]);
#endif

	FLOAT residu = 0;
	FLOAT sum_c = 0;
	for (int j = 0; j < n_pts; j++) {
		FLOAT c = (confs == NULL ? 1 : confs[j]);
		residu +=
		    c * fabs(ranges[j] -
			     distance(positions[n_pts], positions[j]));
		sum_c += c;
	}
	residu /= sum_c;

#ifndef NDEBUG
	EV << " RESIDU = " << residu << " ERR = " <<
	    100 * distance(positions[n_pts],
			   node[target].true_pos) / range << "%\n";
#endif
	return residu;
}


FLOAT PositifLayer::hoptriangulate(int n_pts, FLOAT ** positions,
				   FLOAT * ranges, int target)
{
	FLOAT est_R;

	if (n_pts <= nr_dims + 1)
		return false;

	::params.dim = nr_dims;
	::params.alg_sel = 0;
	::params.conf_mets = false;
	if (!::hoptriangulate(&::params, n_pts, positions, ranges, NULL,
			      positions[n_pts], &est_R)) {
#ifndef NDEBUG
		EV << ": FAILED TRIANGULATION\n";
#endif
		return -1;
	}
#ifndef NDEBUG
	EV << ":A " << pos2str(positions[n_pts]);
#endif

	// Run sanity check
	for (int i = 0; i < n_pts; i++)
		ranges[i] *= est_R;
	FLOAT correction;
	::hoptriangulate(&::params, n_pts + 1, positions, ranges, NULL,
			 positions[n_pts], &correction);
#ifndef NDEBUG
	EV << ":B " << pos2str(positions[n_pts]);
#endif

	return (correction < .9 || correction > 1.1 ? -1 : correction * est_R);
}

/*******************************************************************************
 * Output statistics methods
 ******************************************************************************/
void PositifLayer::write_statistics()
{
	//////// START OF OUTPUT CODE /////////

	// Print this twice for convience. (Once more below)
	statistics(false);

	// Collect final positions
	int stopped = 0;
	cStdDev errs_phase1;
	cStdDev errs_phase2;
	cStdDev errs;
	cStdDev bad_node_pos_errs;
	cStdDev avg_flops;
	cStdDev avg_conf;
	cStdDev bcast_unique[MAX_MSG_TYPES];
	cStdDev bcast_total[MAX_MSG_TYPES];
	cStdDev bcast_sum_unique;
	cStdDev bcast_sum_total;
	cStdDev real_range_error;
	cStdDev real_abs_range_error;
	float anchor_range_error = 0;
	float rel_anchor_range_error = 0;
	float abs_anchor_range_error = 0;
	float abs_rel_anchor_range_error = 0;
	float anchor_range = 0;
	int anchor_range_error_count = 0;
	int nr_retries = 0;

	for (int src = 0; src < num_nodes; src++) {

		avg_flops += node[src].perf_data.flops;
		if (node[src].perf_data.phase1_err >= 0)
			errs_phase1 += node[src].perf_data.phase1_err;
		if (node[src].perf_data.phase2_err >= 0)
			errs_phase2 += node[src].perf_data.phase2_err;
		// Only measure the error and confidence for nodes that have a position
		if (node[src].perf_data.status == STATUS_POSITIONED) {
			avg_conf += node[src].perf_data.confidence;
			if (node[src].perf_data.err >= 0)
				errs += node[src].perf_data.err;
		} else if (node[src].perf_data.status == STATUS_BAD)
			bad_node_pos_errs += node[src].perf_data.err;


		// The number of retries is counted in the position of the START message, which is useless to count.
		nr_retries += node[src].perf_data.bcast_total[0];
		int bcast_total_tmp;
		int bcast_unique_tmp;
		bcast_total_tmp = 0;
		bcast_unique_tmp = 0;
		for (int i = MSG_TYPE_BASE; i < MAX_MSG_TYPES; i++) {
			bcast_unique[i] += node[src].perf_data.bcast_unique[i];
			bcast_total[i] += node[src].perf_data.bcast_total[i];
			bcast_unique_tmp += node[src].perf_data.bcast_unique[i];
			bcast_total_tmp += node[src].perf_data.bcast_total[i];
		}
		bcast_sum_unique += bcast_unique_tmp;
		bcast_sum_total += bcast_total_tmp;

		if (node[src].perf_data.status == STATUS_POSITIONED) {
			anchor_range_error +=
			    node[src].perf_data.anchor_range_error *
			    node[src].perf_data.anchor_range_error_count;
			rel_anchor_range_error +=
			    node[src].perf_data.rel_anchor_range_error *
			    node[src].perf_data.anchor_range_error_count;
			abs_anchor_range_error +=
			    node[src].perf_data.abs_anchor_range_error *
			    node[src].perf_data.anchor_range_error_count;
			abs_rel_anchor_range_error +=
			    node[src].perf_data.
			    abs_rel_anchor_range_error *
			    node[src].perf_data.anchor_range_error_count;
			for (int i = 0;
			     i < node[src].perf_data.used_anchors; i++) {
				real_range_error +=
				    (node[src].perf_data.range_list[i] -
				     node[src].perf_data.
				     real_range_list[i]) /
				    node[src].perf_data.real_range_list[i];
				real_abs_range_error +=
				    fabs(node[src].perf_data.
					 range_list[i] -
					 node[src].perf_data.
					 real_range_list[i]) /
				    node[src].perf_data.real_range_list[i];
			}
			anchor_range +=
			    node[src].perf_data.anchor_range *
			    node[src].perf_data.anchor_range_error_count;
			anchor_range_error_count +=
			    node[src].perf_data.anchor_range_error_count;
		}

		char finalposstr[20];	// Should be enough
		char trueposstr[20];	// Should be enough

		strcpy(finalposstr, pos2str(node[src].perf_data.curr_pos));
		strcpy(trueposstr, pos2str(node[src].true_pos));

		fprintf(stderr,
			"%3d: pos = %16s truepos = %16s %10s err = %6.2f%% (p1 %6.2f%% p2 %6.2f%%) conf = %.3f re= %3.1f %3.1f %3.1f\n",
			node[src].ID, finalposstr, trueposstr,
			status2string(node[src].perf_data.status),
			100 * distance(node[src].perf_data.curr_pos,
				       node[src].true_pos) / range,
			node[src].perf_data.phase1_err * 100,
			node[src].perf_data.phase2_err * 100,
			node[src].perf_data.confidence,
			node[src].perf_data.anchor_range_error,
			node[src].perf_data.abs_anchor_range_error,
			node[src].perf_data.anchor_range);

		stopped++;
	}

	if (errs_phase1.samples() > 0)
		fprintf(stderr,
			"avg 1st phase error (%d nodes): %4.1f%% (+/- %3.1f%%)\n",
			(int) errs_phase1.samples(), 100 * errs_phase1.mean(),
			100 * errs_phase1.stddev());
	if (errs_phase2.samples() > 0)
		fprintf(stderr,
			"avg 2nd phase error (%d nodes): %4.1f%% (+/- %3.1f%%)\n",
			(int) errs_phase2.samples(), 100 * errs_phase2.mean(),
			100 * errs_phase2.stddev());
	if (errs.samples() > 0)
		fprintf(stderr,
			"avg final error (%d nodes): %4.1f%% (+/- %3.1f%%)\n",
			(int) errs.samples(), 100 * errs.mean(),
			100 * errs.stddev());

	if (bad_node_pos_errs.samples() > 0)
		fprintf(stderr,
			"avg error of bad nodes (%d nodes): %4.1f%% (+/- %3.1f%%)\n",
			(int) bad_node_pos_errs.samples(),
			100 * bad_node_pos_errs.mean(),
			100 * bad_node_pos_errs.stddev());

	fprintf(stderr, "avg confidence: %.3f +/- %.3f\n",
		avg_conf.mean(), avg_conf.stddev());
	fprintf(stderr, "avg flops: %f +/- %.3f\n",
		avg_flops.mean(), avg_flops.stddev());

	statistics(false);

	fprintf(stderr, "\nNumber of RETRY messages: %d\n", nr_retries);
	fprintf(stderr,
		"Message type, bcasts/node, unique bcasts/node, total, total unique\n");
	for (int i = MSG_TYPE_BASE; i < MAX_MSG_TYPES; i++)
		if (bcast_total[i].sum() > 0)
			fprintf(stderr,
				"%8d, %7.1f (+/- %4.1f), %7.1f (+/- %4.1f), %8d, %8d\n",
				i, bcast_total[i].mean(),
				bcast_total[i].stddev(), bcast_unique[i].mean(),
				bcast_unique[i].stddev(),
				(int) bcast_sum_total.sum(),
				(int) bcast_sum_unique.sum());

	for (int n = 0; n < num_nodes; n++)
		if (strlen(node[n].perf_data.log) > 0)
			fprintf(stderr, "---%d\n%s\n", n,
				node[n].perf_data.log);

	//
	// Print raw data stuff
	//

	int count_anchor = 0;
	int count_unknown = 0;
	int count_positioned = 0;
	int count_bad = 0;
	//int flop_count=0;
	//int bcast_count=0;
	cStdDev connectivity;
	for (int i = 0; i < num_nodes; i++) {
		switch (node[i].perf_data.status) {
		case STATUS_ANCHOR:
			count_anchor++;
			break;
		case STATUS_UNKNOWN:
			count_unknown++;
			break;
		case STATUS_POSITIONED:
			count_positioned++;
			break;
		case STATUS_BAD:
			count_bad++;
			break;
		}
		connectivity += node[i].recv_cnt;
	}

	/*
	   %d %f %f
	   %d %f %f 
	   %d %f %f 
	   %d %f %f 
	   %d %f %f 
	   %f %f 
	   %f %f 
	   %f %f 
	   %f %f 
	   %d 
	   %d 
	   %d 
	   %d 
	   %d 
	 */
	fprintf(stderr,
		"random seed, algorithm, range, range variance, anch_frac, num_nodes, density, connectivity, final samples/mean err/stddev, 1st phase s/m/s, 2nd phase s/m/s, bad nodes s/m/s, conf m/s, flops m/s, bcast tot m/s, bcast uniq m/s, bcast t5 m/s, t6 m/s, t7 m/s, t8 m/s, t9 m/s, retries, count ANC/UNK/POS/BAD, algorithm version, do_2nd_phase, phase1_anchors, topology\n");
	fprintf(stderr,
		"RAWDATA %d %f %f %f %d %f %f %d %f %f %d %f %f %d %f %f %d %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %d %d %d %d %d %d %d %d %d %d %s %f %f %f %f %f %f %f %f %f %f %d %f %f %f %f %d\n",
		// Parameters and derived statistics
		algorithm,
		range,
		range_variance,
		(double) num_anchors / (double) num_nodes,
		num_nodes, num_nodes / area, connectivity.mean(),
		// Sim results
		(int) errs.samples(), 100 * errs.mean(), 100 * errs.stddev(),
		(int) errs_phase1.samples(), 100 * errs_phase1.mean(),
		100 * errs_phase1.stddev(), (int) errs_phase2.samples(),
		100 * errs_phase2.mean(), 100 * errs_phase2.stddev(),
		(int) bad_node_pos_errs.samples(),
		100 * bad_node_pos_errs.mean(),
		100 * bad_node_pos_errs.stddev(), avg_conf.mean(),
		avg_conf.stddev(), avg_flops.mean(), avg_flops.stddev(),
		bcast_sum_total.mean(), bcast_sum_total.stddev(),
		bcast_sum_unique.mean(), bcast_sum_unique.stddev(),
		bcast_total[5].mean(), bcast_total[5].stddev(),
		bcast_total[6].mean(), bcast_total[6].stddev(),
		bcast_total[7].mean(), bcast_total[7].stddev(),
		bcast_total[8].mean(), bcast_total[8].stddev(),
		bcast_total[9].mean(), bcast_total[9].stddev(), nr_retries,
		count_anchor, count_unknown, count_positioned, count_bad,
		version, do_2nd_phase ? 1 : 0, phase1_min_anchors,
		phase1_max_anchors, flood_limit, topology_type, pos_variance,
		var0, var1, var2,
		(double) count_positioned / (double) (count_unknown +
						      count_positioned +
						      count_bad),
		anchor_range_error / anchor_range_error_count,
		rel_anchor_range_error / anchor_range_error_count,
		abs_anchor_range_error / anchor_range_error_count,
		abs_rel_anchor_range_error / anchor_range_error_count,
		anchor_range / anchor_range_error_count, tri_alg,
		real_range_error.mean(), real_range_error.stddev(),
		real_abs_range_error.mean(), real_abs_range_error.stddev(),
		refine_limit);
}

void PositifLayer::statistics(bool heading)
{
	cStdDev connectivity;
	cStdDev pos_errs;
	cStdDev avg_conf;
	int count_anchor = 0;
	int count_unknown = 0;
	int count_positioned = 0;
	int count_bad = 0;
	int flop_count = 0;
	int bcast_count = 0;

	// collect statistics
	for (int i = 0; i < num_nodes; i++) {
		// Only measure confidence and error for nodes with a position
		if (node[i].perf_data.status == STATUS_POSITIONED) {
			pos_errs +=
			    distance(node[i].true_pos,
				     node[i].perf_data.curr_pos);
			avg_conf += node[i].perf_data.confidence;
		}

		connectivity += node[i].recv_cnt;
		flop_count += node[i].perf_data.flops;
		switch (node[i].perf_data.status) {
		case STATUS_ANCHOR:
			count_anchor++;
			break;
		case STATUS_UNKNOWN:
			count_unknown++;
			break;
		case STATUS_POSITIONED:
			count_positioned++;
			break;
		case STATUS_BAD:
			count_bad++;
			break;
		}
		for (int j = MSG_TYPE_BASE; j < MAX_MSG_TYPES; j++)
			bcast_count += node[i].perf_data.bcast_total[j];
	}

	if (heading) {
		fprintf(stderr, "\nSTATISTICS\n");
		fprintf(stderr, "  #nodes      : %d\n", num_nodes);
		fprintf(stderr, "  density     : %.2f /m^%d\n",
			num_nodes / area, nr_dims);
		fprintf(stderr, "  #anchors    : %d (%d%%)\n", num_anchors,
			(100 * num_anchors) / num_nodes);
		fprintf(stderr, "  radio range : %g m\n", range);
		fprintf(stderr, "  connectivity: %.2f +/- %.2f\n",
			connectivity.mean(), connectivity.stddev());
	}

	if (pos_errs.samples() > 0) {
		fprintf(stderr, "t = %4d #bcast %6d, ", (int) simTime(),
			bcast_count);
		fprintf(stderr, " #flops %6d, ", flop_count);
		fprintf(stderr, " ANC%4d, UNK%4d, POS%4d, BAD%4d, TOT%4d, ",
			count_anchor, count_unknown, count_positioned,
			count_bad, num_nodes);
		fprintf(stderr, "err (/R): %6.2f%% +/- %5.2f, ",
			100 * pos_errs.mean() / range,
			100 * pos_errs.stddev() / range);
		fprintf(stderr, "conf: %1.3f +/- %1.3f\n", avg_conf.mean(),
			avg_conf.stddev());
	}
}

void PositifLayer::write_configuration(const char * filename)
{
	FLOAT bound;
	FILE *fp = fopen(filename, "w");

	if (fp == NULL) {
		error("Can't open '%s'", filename);
		return;
	}

	/* determine bound */
	bound = dim[0];
	for (int d = 1; d < nr_dims; d++) {
		if (dim[d] > bound)
			bound = dim[d];
	}

        const char *prelude[] = {
                "# Topology description:",
                "#   'nr-dimensions'     are we describing a 2-D or a 3-D topology?",
                "#   'grid-bound'        the network is contained in a box of size",
                "#                           [0:grid-bound] x [0:grid-bound] (x [0:grid-bound])",
                "#   'radio-range'       the maximum length of a connection between two nodes",
                "#   'connection'        a radio link between a source and destination node.",
                "#                           Note that connections are directed and there",
                "#                           need NOT be a reverse connection.",
                "#   'measured-range'    (noisy) distance estimate obtained by receiver on",
                "#                           a connection (same for each message).",
                "#   'nr-nodes'          the number of nodes in the network",
                "#   'nr-anchors'        the number of anchors in the network. An anchor is a",
                "#                           node that 'knows' its true position, others do not.",
                "#   'ID'                node identification (unsigned int), may be outside",
                "#                           range [0:'nr-nodes'-1]",
                "#   'position'          2 (or 3) floating point numbers",
                NULL
        };

	for (int i = 0; prelude[i] != NULL; i++) {
		fprintf(fp, "%s\n", prelude[i]);
	}
	fprintf(fp, "\n");

	fprintf(fp, "# nr-dimensions grid-bound radio-range\n%d %g %g\n\n", nr_dims, bound, range);
	fprintf(fp, "# nr-nodes nr-anchors\n%d %d\n\n", num_nodes, num_anchors);
	fprintf(fp, "# positions: 'nr-nodes' lines with <ID> <x-coord> <y-coord> [<z-coord>]\n");

        // Positions
        for (int n = 0; n < num_nodes; n++) {
                fprintf(fp, "%d", node[n].ID);
                if (node[n].anchor) {
                        for (int d = 0; d < nr_dims; d++) {
                                fprintf(fp, " %g", node[n].true_pos[d]);
                        }
                        fprintf(fp, " # ANCHOR");
                } else {
                        for (int d = 0; d < nr_dims; d++) {
                                fprintf(fp, " %g", node[n].true_pos[d]);
                        }
//                         if (skip[n]) {
//                                 fprintf(fp, " # UNDETERMINED");
//                         } else
// 			{
//                                 if (ginfo[n].twin >= 0) {
//                                         fprintf(fp, " # TWIN %d", node[ginfo[n].twin].ID);
//                                 } else if (bad[n]) {
//                                         fprintf(fp, " # DEPENDENT");
//                                 }
//                         }
                        // Obtain stats about neighbors
                        int n_ok = 0;
                        int n_bad = 0;
                        int n_undetermined = 0;
                        for (cLinkedListIterator iter(*node[n].neighbors); !iter.end(); iter++) {
//                                 int m = ((neighbor_info *) iter())->idx;

//                                 if (skip[m])
//                                         n_undetermined++;
//                                 else if (bad[m])
//                                         n_bad++;
//                                 else
                                        n_ok++;
                        }
                        if (n_ok <= nr_dims) {
                                fprintf(fp, " (nghbrs:");
                                if (n_ok > 0) {
                                        fprintf(fp, " %d ok", n_ok);
                                }
                                if (n_bad > 0) {
                                        fprintf(fp, " %d bad", n_bad);
                                }
                                if (n_undetermined > 0) {
                                        fprintf(fp, " %d undetermined", n_undetermined);
                                }
                                fprintf(fp, ")");
                        }
                }
                fprintf(fp, "\n");
        }

        fprintf(fp, "\n# anchor-list: 'nr-anchor's IDs\n");
        bool first = true;
        for (int n = 0; n < num_nodes; n++) {
                if (node[n].anchor) {
                        if (first) {
                                first = false;
                        } else {
                                fprintf(fp, " ");
                        }
                        fprintf(fp, "%d", node[n].ID);
                }
        }
        fprintf(fp, "\n");

        fprintf(fp, "\n# connections: src dst measured-range\n");
        for (int i = 0; i < num_nodes; i++) {
                for (cLinkedListIterator iter(*node[i].neighbors); !iter.end(); iter++) {
                        neighbor_info *n = (neighbor_info *) iter();

                        fprintf(fp, "%d %d %g\n", node[i].ID, node[n->idx].ID, n->est_dist);
                }
        }
	fclose(fp);
}

/*******************************************************************************
 * topology methods
 ******************************************************************************/

static float RANGE_DIV_1000;

void PositifLayer::analyzeTopology(void)
{
	ginfo = new glob_info[num_nodes];
	bool skip[num_nodes];
	bool bad[num_nodes];

	scenario = fopen(par("outputfile"), "w");
//	scenario = stdout;
	if (scenario == NULL) {
		error("can't open file for logging scenario");
		scenario = fopen("/dev/null", "w");
	}
	assert(scenario != NULL);

	fprintf(scenario, "# Prelude: analysis results\n");

	RANGE_DIV_1000 = range / 1000;

	for (int n = 0; n < num_nodes; n++) {
		skip[n] = false;
	}
	prune_loose_nodes(skip);

	for (int n = 0; n < num_nodes; n++) {
		ginfo[n].idx = n;
		ginfo[n].stuck = false;
		ginfo[n].twin = -1;
		memcpy(ginfo[n].init_pos, node[n].init_pos,
		       sizeof(node[n].init_pos));
		if (node[n].anchor) {
			ginfo[n].type = Anchor;
			ginfo[n].conf = 1;
		} else if (skip[n]) {
			ginfo[n].type = Skip;
		} else {
			ginfo[n].type = Unknown;
			ginfo[n].conf = LOW_CONF;
		}
	}

	run_terrain(strcmp(par("terrain"), "NO") == 0, skip);

	for (int n = 0; n < num_nodes; n++) {
		bad[n] = skip[n];
	}

	find_bad_nodes(bad);

	fprintf(stderr, "\nBAD NODES:");
	for (int n = 0; n < num_nodes; n++) {
		if (ginfo[n].type == Unknown && bad[n]) {
			ginfo[n].bad = true;
			fprintf(stderr, " %d", node[n].ID);
			ginfo[n].conf = ZERO_CONF;
		} else {
			ginfo[n].bad = false;
		}
	}
	fprintf(stderr, "\n");

	topology_stats(skip);

	save_scenario(skip, bad);

	fclose(scenario);

	//run_algorithms();
}


/*static bool desc_gain(void *a, void *b)
{
	return ((glob_info *) a)->gain >= ((glob_info *) b)->gain;
}
*/


static bool inc_err(void *a, void *b)
{
	if (((glob_info *) b)->gain <= RANGE_DIV_1000)
		return true;

	return (((glob_info *) a)->gain > RANGE_DIV_1000)
	    && (((glob_info *) a)->next.err < ((glob_info *) b)->next.err);
}


void PositifLayer::run_algorithms(void)
{
#if 0
	// Try to estimate the best refinement order possible.
	// Compute the potential improvement of each node when performing a
	// triangulation. Always advange the node with the largest gain.

	SortedList list;
	list.order(&desc_gain);

	for (int n = 0; n < num_nodes; n++) {
		if (ginfo[n].type == Stuck)
			ginfo[n].type = Unknown;
		ginfo[n].curr.pos = ginfo[n].init_pos;
		ginfo[n].curr.err =
		    distance(node[n].true_pos, ginfo[n].curr.pos);
		ginfo[n].curr.conf = ginfo[n].conf;
	}

	for (int n = 0; n < num_nodes; n++) {
		if (!node[n].anchor && node[n].recv_cnt > nr_dims) {
			glob_triangulate(&ginfo[n]);
			list.insert(&ginfo[n]);
		}
	}

	for (;;) {
		glob_info *hd = (glob_info *) list.head();

		// Are we done?
		if (hd->gain <= range / 1000) {
			break;
		}
//EV << "advance " << node[hd->idx].ID << ": " << pos2str(hd->curr.pos) << " to " << pos2str(hd->next.pos) << ", gain = " << hd->gain << "\n";

//EV << "curr err " << hd->curr.err << " next err " << hd->next.err << "\n";
		// Advance one step
		hd->curr = hd->next;
		hd->gain = 0;
		list.remove((void *) hd);
		list.insert(hd);

		// Update neighbors
		for (cLinkedListIterator iter(node[hd->idx].neighbors);
		     !iter.end(); iter++) {
			glob_info *n = &ginfo[((neighbor_info *) iter())->idx];

			if (ginfo[n->idx].type == Unknown
			    || ginfo[n->idx].type == Stuck) {
				glob_triangulate(n);
				ginfo[n->idx].type =
				    (ginfo[n->idx].stuck ? Stuck : Unknown);
				list.remove((void *) n);
				list.insert(n);
			}
		}
	}
	stats("biggest gain");

	// PositifLayer scheme three. Assume perfect synch, so we can run iterations.
	// In each iteration, only nodes that gain from updating their position
	// do so.
	for (int n = 0; n < num_nodes; n++) {
		if (ginfo[n].type == Stuck)
			ginfo[n].type = Unknown;
		ginfo[n].curr.pos = ginfo[n].init_pos;
		ginfo[n].curr.err =
		    distance(node[n].true_pos, ginfo[n].curr.pos);
		ginfo[n].curr.conf = ginfo[n].conf;
	}
	bool done;
	do {
		for (int n = 0; n < num_nodes; n++) {
			if (ginfo[n].type == Unknown || ginfo[n].type == Stuck) {
				glob_triangulate(&ginfo[n]);
				ginfo[n].type =
				    (ginfo[n].stuck ? Stuck : Unknown);
			}
		}
		done = true;
		for (int n = 0; n < num_nodes; n++) {
			if (ginfo[n].type == Unknown) {
				if (ginfo[n].gain > range / 1000) {
					ginfo[n].curr = ginfo[n].next;
					done = false;
				}
			}
		}
	} while (!done);
	stats("synch iterations");
#endif

	// PositifLayer scheme four. Advance the node whose next position is closest
	// to its true position.
	for (int n = 0; n < num_nodes; n++) {
		if (ginfo[n].type == Stuck)
			ginfo[n].type = Unknown;
		memcpy(ginfo[n].curr.pos, ginfo[n].init_pos,
		       sizeof(ginfo[n].init_pos));
		ginfo[n].curr.err =
		    distance(node[n].true_pos, ginfo[n].curr.pos);
		ginfo[n].curr.conf = ginfo[n].conf;
	}

	SortedList sorted;
	sorted.order(&inc_err);

	for (int n = 0; n < num_nodes; n++) {
		if (!node[n].anchor && node[n].recv_cnt > nr_dims) {
			glob_triangulate(&ginfo[n]);
			sorted.insert(&ginfo[n]);
		}
	}

	for (;;) {
		glob_info *hd = (glob_info *) sorted.head();

		// Are we done?
		if (hd->gain <= range / 1000) {
			break;
		}
//EV << "ADVANCE " << node[hd->idx].ID << ": " << pos2str(hd->curr.pos) << " to " << pos2str(hd->next.pos) << ", gain = " << hd->gain << "\n";

//EV << "curr err " << hd->curr.err << " next err " << hd->next.err << "\n";
		// Advance one step
		hd->curr = hd->next;
		hd->gain = 0;
		sorted.remove(hd);
		sorted.insert(hd);

		// Update neighbors
		for (cLinkedListIterator iter(*(node[hd->idx].neighbors));
		     !iter.end(); iter++) {
			glob_info *n = &ginfo[((neighbor_info *) iter())->idx];

			if (ginfo[n->idx].type == Unknown
			    || ginfo[n->idx].type == Stuck) {
				glob_triangulate(n);
				ginfo[n->idx].type =
				    (ginfo[n->idx].stuck ? Stuck : Unknown);
				sorted.remove(n);
				sorted.insert(n);
			}
		}
	}
	stats("closest true move", false);

#if 0
	// PositifLayer scheme five. Advance the nodes in waves from the anchors
	for (int n = 0; n < num_nodes; n++) {
		if (ginfo[n].type == Stuck)
			ginfo[n].type = Unknown;
		ginfo[n].curr.pos = ginfo[n].init_pos;
		ginfo[n].curr.err =
		    distance(node[n].true_pos, ginfo[n].curr.pos);
		ginfo[n].curr.conf = ginfo[n].conf;
	}
	// set up a wave
	for (int n = 0; n < num_nodes; n++) {
		ginfo[n].wave_cnt = num_nodes + 100;
	}
	for (int n = 0; n < num_nodes; n++) {
		if (node[n].anchor) {
			ginfo[n].wave_cnt = 0;
			for (cLinkedListIterator iter(node[n].neighbors);
			     !iter.end(); iter++) {
				neighbor_info *nbr = (neighbor_info *) iter();
				if (!node[nbr->idx].anchor)
					ginfo[nbr->idx].wave_cnt = 1;
			}
		}
	}
	bool end_of_wave;
	int max_hop = 0;
	do {
		// propagate wave one hop
		max_hop++;
		end_of_wave = true;
		for (int n = 0; n < num_nodes; n++) {
			if (ginfo[n].wave_cnt == max_hop) {
				for (cLinkedListIterator
				     iter(node[n].neighbors); !iter.end();
				     iter++) {
					neighbor_info *nbr =
					    (neighbor_info *) iter();
					if (ginfo[nbr->idx].wave_cnt >
					    num_nodes) {
						ginfo[nbr->idx].wave_cnt =
						    max_hop + 1;
						end_of_wave = false;
					}
				}
			}
		}
	} while (!end_of_wave);

//EV << "wavefront: max_hop = " << max_hop << "\n";

	do {
		done = true;
		for (int hops = 1; hops <= max_hop; hops++) {
			// Nodes at wavefront compute position updates
			for (int n = 0; n < num_nodes; n++) {
				if (ginfo[n].wave_cnt == hops
				    && (ginfo[n].type == Unknown
					|| ginfo[n].type == Stuck)) {
					glob_triangulate(&ginfo[n]);
					ginfo[n].type =
					    (ginfo[n].stuck ? Stuck : Unknown);
				}
			}

			// Execute updates concurrently
			for (int n = 0; n < num_nodes; n++) {
				if (ginfo[n].wave_cnt == hops) {
					if (ginfo[n].type == Unknown
					    && ginfo[n].gain > range / 1000) {
//EV << "ADVANCE " << node[n].ID << ": " << pos2str(ginfo[n].curr.pos) << " to " << pos2str(ginfo[n].next.pos) << ", gain = " << ginfo[n].gain << "\n";
						ginfo[n].curr = ginfo[n].next;
						done = false;
					}
				}
			}
		}
	} while (!done);
	stats("anchor waves");
#endif
}



void PositifLayer::stats(const char *str, bool details)
{
	cStdDev errs;
	cStdDev clean_errs;
	int stuck = 0;
	int skip = 0;

	fprintf(stderr, "\n");
	for (int n = 0; n < num_nodes; n++) {
		switch (ginfo[n].type) {
		case Anchor:
			break;

		case Skip:
			skip++;
			break;

		case Stuck:
			assert(ginfo[n].stuck);
		case Unknown:
			if (details) {
				fprintf(stderr, "%3d: pos = %13s err = %6.2f",
					node[n].ID, pos2str(ginfo[n].curr.pos),
					100 * ginfo[n].curr.err / range);
				fprintf(stderr, "\tnext = %13s err = %6.2f",
					pos2str(ginfo[n].next.pos),
					100 * ginfo[n].next.err / range);
			}
			if (ginfo[n].stuck) {
				if (details)
					fprintf(stderr, " STUCK");
				stuck++;
			} else {
				errs += ginfo[n].curr.err;
				if (!ginfo[n].bad) {
					clean_errs += ginfo[n].curr.err;
				}
			}
			if (details)
				fprintf(stderr, "\n");
			break;

		default:
			error("stats: unexpected node type '%d'",
			      ginfo[n].type);
			break;
		}
	}
	fprintf(stderr,
		"GLOB (%-17s) skip %d, stuck %d, nodes %d (%d bad), error %.2f%% (%.2f)\n",
		str, skip, stuck, (int) errs.samples(),
		(int) errs.samples() - (int) clean_errs.samples(),
		100 * errs.mean() / range, 100 * clean_errs.mean() / range);
}



void PositifLayer::glob_triangulate(glob_info * nd)
{
	int n = node[nd->idx].neighbors->length();
	FLOAT *pos_list[n + 1];
	FLOAT range_list[n + 1];
	FLOAT weights[n];
	FLOAT sum_conf = 0;

	int i = 0;
	for (cLinkedListIterator iter(*(node[nd->idx].neighbors)); !iter.end();
	     iter++) {
		neighbor_info *neighbor = (neighbor_info *) iter();

		if (ginfo[neighbor->idx].type != Skip
		    && ginfo[neighbor->idx].curr.conf > ZERO_CONF) {
			pos_list[i] = ginfo[neighbor->idx].curr.pos;
			range_list[i] = neighbor->est_dist;
			weights[i] = ginfo[neighbor->idx].curr.conf;
			sum_conf += weights[i];
			i++;
		}
	}
#ifndef NDEBUG
	EV << node[nd->idx].ID << ": triangulate with " << i << " nodes\n";
#endif

	pos_list[i] = nd->next.pos;
	FLOAT res = triangulate(i, pos_list, range_list, weights, nd->idx);

	if (res < 0 || res > range) {
		// ignore it
#ifndef NDEBUG
		EV << node[nd->idx].ID << ": IGNORE triangulation\n";
#endif
		nd->stuck = true;
		nd->next = nd->curr;
		nd->next.conf = ZERO_CONF;
		nd->gain = 0;
	} else {
		nd->stuck = false;
		nd->next.err = distance(node[nd->idx].true_pos, nd->next.pos);
		nd->next.conf = sum_conf / i;
		nd->gain = nd->curr.err - nd->next.err;
#ifndef NDEBUG
		EV << node[nd->idx].ID << ": gain = " << nd->
		    gain << ", next confidence = " << nd->next.conf << "\n";
#endif
	}
}


void PositifLayer::run_terrain(bool stats_only, bool * undetermined)
{
	int hop_cnt[num_nodes][num_anchors];
	int path[num_nodes][num_anchors];
	int anchor_idx[num_anchors];
	int entries[num_nodes];
	int anchor = 0;
	int hop;
	int front_cnt;
	bool changed;

	for (int n = 0; n < num_nodes; n++) {
		if (node[n].anchor) {
			// flood from this anchor
			for (int m = 0; m < num_nodes; m++) {
				hop_cnt[m][anchor] = num_nodes;
			}
			hop_cnt[n][anchor] = 0;
			path[n][anchor] = n;
			hop = 0;
			do {
				// flood one step from wave front
				front_cnt = 0;
				for (int m = 0; m < num_nodes; m++) {
					if (hop_cnt[m][anchor] == hop) {
						for (cLinkedListIterator
						     iter(*(node[m].neighbors));
						     !iter.end(); iter++) {
							neighbor_info *nbr =
							    (neighbor_info *)
							    iter();

							if (hop_cnt[nbr->idx]
							    [anchor] ==
							    num_nodes) {
								hop_cnt[nbr->
									idx]
								    [anchor] =
								    hop + 1;
								path[nbr->
								     idx]
								    [anchor] =
								    m;
								front_cnt++;
							}
						}
					}
				}
				hop++;
			} while (front_cnt > 0);
			anchor_idx[anchor++] = n;
		}
	}

#ifndef NDEBUG
	fprintf(stderr, "\nANCHOR DISTANCE TABLE\nnode ");
	for (int a = 0; a < num_anchors; a++) {
		fprintf(stderr, "%4d", node[anchor_idx[a]].ID);
	}
	fprintf(stderr, "  min  avg  max  entries\n");
#endif
	for (int n = 0; n < num_nodes; n++) {
		if (!node[n].anchor) {
			int min = num_nodes, max = 0;
			int n_anch = 0;
			float sum = 0;

			entries[n] = 0;
#ifndef NDEBUG
			fprintf(stderr, "%4d%c", n,
				node[n].recv_cnt <= nr_dims ? '*' : ' ');
#endif
			for (int a = 0; a < num_anchors; a++) {
				if (hop_cnt[n][a] < num_nodes) {
					n_anch++;
#ifndef NDEBUG
					fprintf(stderr, "%4d", hop_cnt[n][a]);
#endif
					sum += hop_cnt[n][a];
					if (hop_cnt[n][a] < min)
						min = hop_cnt[n][a];
					if (hop_cnt[n][a] > max)
						max = hop_cnt[n][a];

					bool unique = true;
					for (int b = 0; b < a; b++) {
						if (path[n][a] == path[n][b]) {
							unique = false;
							break;
						}
					}
					if (unique)
						entries[n]++;
				} else {
#ifndef NDEBUG
					fprintf(stderr, "%4s", "");
#endif
				}
			}
#ifndef NDEBUG
			if (n_anch <= nr_dims) {
				fprintf(stderr, "  -- disconnected --\n");
			} else {
				fprintf(stderr, "%5d%5.1f%5d%6d\n", min,
					sum / n_anch, max, entries[n]);
			}
#endif
		}
	}
#ifndef NDEBUG
	fprintf(stderr, "\n");
#endif

	// A good location can only be obtained if a node is connected to
	// enough anchors AND the paths to these anchors do not overlap.
	// An example bad case is when the network can be partitioned by cutting
	// 1 (or 2) links and some part has less than 1 (0) anchors in it.
	// We determine the fitness of a node as follows:
	//  - all anchors are fit
	//  - all nodes with 3 or more (different) anchor entries are fit
	//  - a node is fit if the size of the set of entries and fit neighbors
	//    exceeds 2

	// Phase 1: be optimistic assume that entry points are reachable through
	// determined nodes

	int links[num_nodes];
	for (int n = 0; n < num_nodes; n++) {
		undetermined[n] = !node[n].anchor && entries[n] <= nr_dims;
	}
	do {
		changed = false;
		for (int n = 0; n < num_nodes; n++) {
			if (undetermined[n]) {
				links[n] = entries[n];
			}
		}
		for (int n = 0; n < num_nodes; n++) {
			if (!undetermined[n]) {
				for (cLinkedListIterator
				     iter(*(node[n].neighbors)); !iter.end();
				     iter++) {
					int m = ((neighbor_info *) iter())->idx;

					if (undetermined[m]) {
						// Can we add a new link to this undetermined node?
						bool unique = true;
						for (int a = 0; a < num_anchors;
						     a++) {
							if (path[m][a] == n) {
								unique = false;
								break;
							}
						}
						if (unique) {
							links[m]++;
							if (links[m] > nr_dims) {
								undetermined[m]
								    = false;
								changed = true;
							}
						}
					}
				}
			}
		}
	} while (changed);

	// Phase 2: be realistic and prune all nodes that do not have enough
	// determined neighbours.

	int cnt[num_nodes];
	do {
		changed = false;
		for (int n = 0; n < num_nodes; n++) {
			cnt[n] = 0;
		}
		for (int n = 0; n < num_nodes; n++) {
			if (!undetermined[n]) {
				for (cLinkedListIterator
				     iter(*(node[n].neighbors)); !iter.end();
				     iter++) {
					int m = ((neighbor_info *) iter())->idx;

					cnt[m]++;
				}
			}
		}
		for (int n = 0; n < num_nodes; n++) {
			if (!undetermined[n] && !node[n].anchor
			    && cnt[n] <= nr_dims) {
				undetermined[n] = true;
				changed = true;
			}
		}
	} while (changed);

	fprintf(stderr, "UNDETERMINED nodes:");
	for (int n = 0; n < num_nodes; n++) {
		if (undetermined[n]) {
			fprintf(stderr, "%4d", node[n].ID);
		}
	}
	fprintf(stderr, "\n");

	if (stats_only)
		return;

	int n_hops = 0;
	FLOAT sum_dist = 0;
	for (int a = 0; a < num_anchors; a++) {
		for (int b = a + 1; b < num_anchors; b++) {
			if (hop_cnt[a][b] < num_nodes) {
				sum_dist +=
				    distance(node[anchor_idx[a]].true_pos,
					     node[anchor_idx[b]].true_pos);
				n_hops += hop_cnt[a][b];
			}
		}
	}

	FLOAT hop_dist = sum_dist / n_hops;

	FLOAT *pos_list[num_anchors + 1];
	FLOAT range_list[num_anchors + 1];

	cStdDev pos_errs;

#ifndef NDEBUG
	fprintf(stderr, "TERRAIN results: avg hop dist = %6.2f (%6.2f%%)\n",
		hop_dist, 100 * hop_dist / range);
#endif
	for (int n = 0; n < num_nodes; n++) {
		if (ginfo[n].type != Anchor) {
			int i = 0;
			for (int a = 0; a < num_anchors; a++) {
				if (hop_cnt[n][a] < num_nodes) {
					pos_list[i] =
					    node[anchor_idx[a]].true_pos;
					range_list[i] = hop_cnt[n][a] * range;	// hop_dist;

					// Only use the N closest anchors for better results
					if (i == nr_dims + 2) {
						int max = 0;

						for (int j = 0; j < i; j++) {
							if (range_list[j] >
							    range_list[max]) {
								max = j;
							}
						}

						if (range_list[i] <
						    range_list[max]) {
							pos_list[max] =
							    pos_list[i];
							range_list[max] =
							    range_list[i];
						}
					} else {
						i++;
					}

				}
			}
			if (i <= nr_dims) {
				assert(undetermined[n]);
				ginfo[n].type = Skip;
			} else {
				pos_list[i] = ginfo[n].init_pos;
				FLOAT res =
				    triangulate(i, pos_list, range_list, NULL,
						n);
				if (0 <= res && res <= range) {
					if (ginfo[n].type == Unknown) {
						pos_errs +=
						    distance(node[n].true_pos,
							     ginfo[n].init_pos);
					}
#ifndef NDEBUG
					fprintf(stderr,
						"%3d%c %13s error %6.2f%%\n",
						node[n].ID,
						undetermined[n] ? '*' : ' ',
						pos2str(ginfo[n].init_pos),
						100 *
						distance(ginfo[n].init_pos,
							 node[n].true_pos) /
						range);
#endif

					// Check if some neighbor has the same position (BAD)
					for (cLinkedListIterator
					     iter(*(node[n].neighbors));
					     !iter.end(); iter++) {
						int m =
						    ((neighbor_info *) iter())->
						    idx;

						if (ginfo[m].type == Unknown
						    && m < n
						    && distance(ginfo[n].
								init_pos,
								ginfo[m].
								init_pos) ==
						    0) {
							ginfo[n].conf =
							    ZERO_CONF;
							break;
						}
					}
				} else {
					ginfo[n].conf = ZERO_CONF;
				}
			}
		}
	}
	fprintf(stderr, "\nTERRAIN's avg err (%d nodes): %.2f%%\n",
		(int) pos_errs.samples(), 100 * pos_errs.mean() / range);

}


void PositifLayer::topology_stats(bool * skip)
{
	cStdDev connectivity;
	int bad = 0;
	int twins = 0;

	// collect statistics
	for (int i = 0; i < num_nodes; i++) {
		if (!node[i].anchor && !skip[i]) {
			if (ginfo[i].twin >= 0) {
				twins++;
			} else if (ginfo[i].bad) {
				bad++;
			}
			connectivity += node[i].recv_cnt;
		}
	}
	int skips = num_nodes - num_anchors - connectivity.samples();

	fprintf(scenario, "#\t#nodes         : %4d\n", num_nodes);
	fprintf(scenario, "#\t  #anchors     : %4d (%2.2f%%)\n", num_anchors,
		(100.0 * num_anchors) / num_nodes);
	fprintf(scenario, "#\t  #undetermined: %4d (%2.2f%%)\n", skips,
		(100.0 * skips) / num_nodes);
	fprintf(scenario, "#\t  #unknowns    : %4d (%2.2f%%),",
		(int) connectivity.samples(),
		(100.0 * connectivity.samples()) / num_nodes);
	fprintf(scenario, " connectivity: %.2f +/- %.2f\n", connectivity.mean(),
		connectivity.stddev());

	fprintf(scenario, "#\t    #twins     : %4d (%2.2f%%)\n", twins,
		(100.0 * twins) / num_nodes);
	fprintf(scenario, "#\t    #dependents: %4d (%2.2f%%)\n\n", bad,
		(100.0 * bad) / num_nodes);

#ifndef NDEBUG
	fprintf(stderr, "\nNEIGHBOR LISTS\n");
	for (int n = 0; n < num_nodes; n++) {
		if (!skip[n]) {
			fprintf(stderr, "%3d:", node[n].ID);
			for (cLinkedListIterator iter(*node[n].neighbors);
			     !iter.end(); iter++) {
				int m = ((neighbor_info *) iter())->idx;

				if (!skip[m]) {
					fprintf(stderr, "%4d", node[m].ID);
				}
			}
			fprintf(stderr, "\n");
		}
	}
#endif
}


void PositifLayer::find_bad_nodes(bool * bad)
{
	while (collapse_twins(bad)) {
		prune_loose_nodes(bad);
	}
}


void PositifLayer::prune_loose_nodes(bool * skip)
{
	bool change;
	int recv_cnt[num_nodes];

	do {
		for (int n = 0; n < num_nodes; n++) {
			recv_cnt[n] = 0;
		}
		for (int n = 0; n < num_nodes; n++) {
			if (!skip[n]) {
				for (cLinkedListIterator
				     iter(*node[n].neighbors); !iter.end();
				     iter++) {
					int m = ((neighbor_info *) iter())->idx;

					recv_cnt[m]++;
				}
			}
		}
		change = false;
		for (int n = 0; n < num_nodes; n++) {
			if (!node[n].anchor && !skip[n]
			    && recv_cnt[n] <= nr_dims) {
				skip[n] = true;
				change = true;
#ifndef NDEBUG
				fprintf(stderr,
					"prune %d, only %d neighbor%s %s\n",
					node[n].ID, recv_cnt[n],
					recv_cnt[n] > 1 ? "s" : "",
					node[n].recv_cnt >
					recv_cnt[n] ? "left" : "");
#endif
			}
		}
	} while (change);
}


bool PositifLayer::collapse_twins(bool * skip)
{
	bool collapsed = false;

	// Go and find "identical twins", which are impossible to localize
	// That is, find eighboring nodes with all of their neighbors in common.
	for (int n = 0; n < num_nodes; n++) {
		bool nghbr_of_n[num_nodes];
		int nghbr_cnt_n = 0;

		if (skip[n] || node[n].anchor)
			continue;

		for (int m = 0; m < num_nodes; m++) {
			nghbr_of_n[m] = false;
		}
		for (cLinkedListIterator iter(*node[n].neighbors); !iter.end();
		     iter++) {
			int m = ((neighbor_info *) iter())->idx;

			if (skip[m])
				continue;

			nghbr_of_n[m] = true;
			nghbr_cnt_n++;
		}

		if (nghbr_cnt_n <= nr_dims) {
			assert(collapsed);
			skip[n] = true;
			fprintf(stderr, "prune %d, only %d neighbor%s %s\n",
				node[n].ID, nghbr_cnt_n,
				nghbr_cnt_n > 1 ? "s" : "",
				node[n].recv_cnt > nghbr_cnt_n ? "left" : "");
			continue;
		}

		for (cLinkedListIterator iter(*node[n].neighbors); !iter.end();
		     iter++) {
			int m = ((neighbor_info *) iter())->idx;
			int nghbr_cnt_m = 0;

			if (skip[m] || node[m].anchor)
				continue;

			// Filter out duplicate twins
			if (m < n)
				continue;

			// Check if all m's neighbors are the same as n's
			bool twins = true;
			for (cLinkedListIterator iter(*node[m].neighbors);
			     !iter.end(); iter++) {
				int k = ((neighbor_info *) iter())->idx;

				if (skip[k])
					continue;

				if (k != n && !nghbr_of_n[k]) {
					twins = false;
					break;
				}
				nghbr_cnt_m++;
			}

			// filter out subsets
			twins &= (nghbr_cnt_n == nghbr_cnt_m);

			if (twins) {
				ginfo[n].twin = m;
				ginfo[m].twin = n;

				// Mark one of the twin as redundant
				skip[m] = true;
				collapsed = true;
				nghbr_cnt_n--;

				fprintf(stderr, "Identical twins: %d and %d\n",
					node[n].ID, node[m].ID);
			}
		}

		// Is 'n' still fit without its twins?
		if (nghbr_cnt_n <= nr_dims) {
			skip[n] = true;
			assert(collapsed);
			fprintf(stderr, "twin %d SHORT on neighbors!!\n",
				node[n].ID);
		}
	}

	return collapsed;
}

void PositifLayer::save_scenario(bool * skip, bool * bad)
{
	char *prelude[] = {
		"# Topology description:",
		"#   'nr-dimensions'     are we describing a 2-D or a 3-D topology?",
		"#   'grid-bound'        the network is contained in a box of size",
		"#                           [0:grid-bound] x [0:grid-bound] (x [0:grid-bound])",
		"#   'radio-range'       the maximum length of a connection between two nodes",
		"#   'connection'        a radio link between a source and destination node.",
		"#                           Note that connections are directed and there",
		"#                           need NOT be a reverse connection.",
		"#   'measured-range'    (noisy) distance estimate obtained by receiver on",
		"#                           a connection (same for each message).",
		"#   'nr-nodes'          the number of nodes in the network",
		"#   'nr-anchors'        the number of anchors in the network. An anchor is a",
		"#                           node that 'knows' its true position, others do not.",
		"#   'ID'                node identification (unsigned int), may be outside",
		"#                           range [0:'nr-nodes'-1]",
		"#   'position'          2 (or 3) floating point numbers",
		NULL
	};

	for (int i = 0; prelude[i] != NULL; i++) {
		fprintf(scenario, "%s\n", prelude[i]);
	}
	fprintf(scenario, "\n");

	FLOAT bound;
	/* determine bound */
	bound = dim[0];
	for (int d = 1; d < nr_dims; d++) {
		if (dim[d] > bound)
			bound = dim[d];
	}

	fprintf(scenario,
		"# nr-dimensions grid-bound radio-range\n%d %g %g\n\n", nr_dims,
		bound, range);
	fprintf(scenario, "# nr-nodes nr-anchors\n%d %d\n\n", num_nodes,
		num_anchors);
	fprintf(scenario,
		"# positions: 'nr-nodes' lines with <ID> <x-coord> <y-coord> [<z-coord>]\n");

	// Positions
	for (int n = 0; n < num_nodes; n++) {
		fprintf(scenario, "%d", node[n].ID);
		if (node[n].anchor) {
			for (int d = 0; d < nr_dims; d++) {
				fprintf(scenario, " %g", node[n].true_pos[d]);
			}
			fprintf(scenario, " # ANCHOR");
		} else {
			for (int d = 0; d < nr_dims; d++) {
				fprintf(scenario, " %g", node[n].true_pos[d]);
			}
			if (skip[n]) {
				fprintf(scenario, " # UNDETERMINED");
			} else {
				if (ginfo[n].twin >= 0) {
					fprintf(scenario, " # TWIN %d",
						node[ginfo[n].twin].ID);
				} else if (bad[n]) {
					fprintf(scenario, " # DEPENDENT");
				}
			}
			// Obtain stats about neighbors
			int n_ok = 0;
			int n_bad = 0;
			int n_undetermined = 0;
			for (cLinkedListIterator iter(*node[n].neighbors);
			     !iter.end(); iter++) {
				int m = ((neighbor_info *) iter())->idx;

				if (skip[m])
					n_undetermined++;
				else if (bad[m])
					n_bad++;
				else
					n_ok++;
			}
			if (n_ok <= nr_dims) {
				fprintf(scenario, " (nghbrs:");
				if (n_ok > 0) {
					fprintf(scenario, " %d ok", n_ok);
				}
				if (n_bad > 0) {
					fprintf(scenario, " %d bad", n_bad);
				}
				if (n_undetermined > 0) {
					fprintf(scenario, " %d undetermined",
						n_undetermined);
				}
				fprintf(scenario, ")");
			}
		}
		fprintf(scenario, "\n");
	}

	fprintf(scenario, "\n# anchor-list: 'nr-anchor's IDs\n");
	bool first = true;
	for (int n = 0; n < num_nodes; n++) {
		if (node[n].anchor) {
			if (first) {
				first = false;
			} else {
				fprintf(scenario, " ");
			}
			fprintf(scenario, "%d", node[n].ID);
		}
	}
	fprintf(scenario, "\n");

	fprintf(scenario, "\n# connections: src dst measured-range\n");
	for (int i = 0; i < num_nodes; i++) {
		for (cLinkedListIterator iter(*node[i].neighbors); !iter.end();
		     iter++) {
			neighbor_info *n = (neighbor_info *) iter();

			fprintf(scenario, "%d %d %g\n", node[i].ID,
				node[n->idx].ID, n->est_dist);
		}
	}
}
