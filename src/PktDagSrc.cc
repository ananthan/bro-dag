// $Id: PktDagSrc.cc 6909 2009-09-10 19:42:19Z vern $
//
// See the file "COPYING" in the main distribution directory for copyright.

extern "C" {
#include <dagapi.h>
#include <dagerf.h>
#include <dagutil.h>
#include <pcap.h>
}
#include <errno.h>

#include <set>
#include <string>

#include "PktDagSrc.h"

using namespace iosource::pktsrc;

// Length of ERF Header before Ethernet header.
#define DAG_ETH_ERFLEN 18

#ifndef ERF_TYPE_META
#define ERF_TYPE_META 27
#endif

static set<string> used_interfaces;

PktDagSrc::PktDagSrc(const std::string& path, bool is_live)
: PktSrc()
	{
	if ( ! is_live )
		Error("pf_ring source does not support offline input");

	current_filter = -1;
	stream_num = 0;
	props.path = path;
	props.is_live = is_live;
	}

void PktDagSrc::Open()
	{
	char interface[DAGNAME_BUFSIZE];
	uint8_t erfs[ERF_TYPE_MAX];
	int types = 0;
	int i = 0;

	dag_parse_name(props.path.c_str(), interface, DAGNAME_BUFSIZE, &stream_num);
	fd = -1;
	// TODO: Won't work for streams, not actually set anywhere? */
	/*
	if ( used_interfaces.find(interface) != used_interfaces.end() )
		{
		Error("DAG interface already in use, can't be used multiple times");
		return;
		}
	*/


	// We only support Ethernet.
	//hdr_size = 14;
	props.link_type = DLT_EN10MB;
	//netmask = 0xffffff00;	// XXX does this make sense?
	props.netmask = NETMASK_UNKNOWN;

	current_filter = -1;

	fd = dag_open(interface);

	// XXX Currently, the DAG fd is not selectable :-(.
	props.selectable_fd = -1;

	if ( fd < 0 )
		{
		Error("dag_open");
		return;
		}

	types = dag_get_stream_erf_types(fd, stream_num, erfs, ERF_TYPE_MAX);
	for (i = 0; i < types; i++)
		{
			dag_record_t dummy;
			dummy.type = erfs[i];
			dummy.rlen = dag_record_size;

			// Annoyingly there isn't a version of this that just takes a type, and dag_get_stream_erf_class_types doesn't currently appear to work with vDAGs.
			if (dagerf_is_ethernet_type((uint8_t*)&dummy))
				{
					break;
				}
		}

	if (types <= 0 || i == types)
		{
			Error("unsupported non-Ethernet DAG link type");
			return;
		}

#if 0
	// long= is needed to prevent the DAG card from truncating jumbo frames.
	char* dag_configure_string =
		copy_string(fmt("slen=%d varlen long=%d",
				snaplen, snaplen > 1500 ? snaplen : 1500));

	fprintf(stderr, "Configuring %s with options \"%s\"...\n",
		interface, dag_configure_string);

	if ( dag_configure(fd, dag_configure_string) < 0 )
		{
		Error("dag_configure");
		delete [] dag_configure_string;
		return;
		}

	delete [] dag_configure_string;
#endif

	if ( dag_attach_stream(fd, stream_num, 0, EXTRA_WINDOW_SIZE) < 0 )
		{
		Error("dag_attach_stream");
		return;
		}

	if ( dag_start_stream(fd, stream_num) < 0 )
		{
		Error("dag_start_stream");
		return;
		}

	struct timeval maxwait, poll;
	maxwait.tv_sec = 0;	// arbitrary due to mindata == 0
	maxwait.tv_usec = 0;
	poll.tv_sec = 0;	// don't wait until more data arrives.
	poll.tv_usec = 0;

	// mindata == 0 for non-blocking.
	if ( dag_set_stream_poll(fd, stream_num, 0, &maxwait, &poll) < 0 )
		{
		Error("dag_set_stream_poll");
		return;
		}

	fprintf(stderr, "listening on DAG card on %s stream %d\n", interface, stream_num);

	stats.link = stats.received = stats.dropped = 0;

	Opened(props);
	}

PktDagSrc::~PktDagSrc()
	{
	}


void PktDagSrc::Close()
	{
	if ( fd >= 0 )
		{
		dag_stop_stream(fd, stream_num);
		dag_detach_stream(fd, stream_num);
		dag_close(fd);
		fd = -1;
		}

	//used_interfaces.erase(interface);
	Closed();
	}

bool PktDagSrc::ExtractNextPacket(Packet* pkt)
	{
	unsigned link_count = 0;	// # packets on link for this call
	const u_char *data = 0;
	pcap_pkthdr hdr;

	// As we can't use select() on the fd, we always have to pretend
	// we're busy (in fact this is probably even true; otherwise
	// we shouldn't be using such expensive monitoring hardware!).
	//idle = false;
	/* TODO: is this still needed? */
	SetIdle(false);

	uint8_t *erf_ptr;
	dag_record_t* r = 0;

	do
		{
		erf_ptr = dag_rx_stream_next_record(fd, 0);
		r = (dag_record_t*) erf_ptr;

		if ( ! r )
			{
			data = 0;	// make dataptr invalid

			if ( errno != EAGAIN )
				{
				Error(fmt("dag_rx_stream_next_record: %s",
						strerror(errno)));
				Close();
				return false;
				}

			else
				{ // gone dry
				SetIdle(true);
				return false;
				}
			}

		// Return after 20 unwanted packets on the link.
		if ( ++link_count > 20 )
			{
			data = 0;
			return false;
			}

		// Locate start of the Ethernet header.

		unsigned int erf_hdr_len = dag_record_size;
		uint8_t erf_type = r->type & 0x7f;

		if (!dagerf_is_color_type(erf_ptr))
			{
			stats.dropped += ntohs(r->lctr);
			}

		if (erf_type == ERF_TYPE_PAD || erf_type == ERF_TYPE_META)
			{
			// Silently skip Provenance and pad records
			continue;
			}

		++stats.link;

		if (dagerf_is_ethernet_type(erf_ptr))
			{
			erf_hdr_len += 2; //eth pad
			}
		else
			{
				Weird(fmt("Unsupported ERF type: %s (%d)", dagerf_type_to_string(r->type, TT_ERROR), erf_type), 0);
				continue;
			}

		if (r->type & 0x80)
			{
			unsigned int num_ext_hdrs = dagerf_ext_header_count(erf_ptr, ntohs(r->rlen));
			if (num_ext_hdrs == 0)
				{
				// TODO: re-arrange so we can supply pkt argument?
				Weird("ERF record with invalid extension header stack", 0);
				continue;
				}

			erf_hdr_len += num_ext_hdrs;
			}

		if (dagerf_is_multichannel_type(erf_ptr))
			erf_hdr_len += 4; //mc_hdr length

		if (erf_hdr_len > ntohs(r->rlen))
			{
			Weird("ERF record with insufficient length for header", 0);
			continue;
			}

		data = (const u_char*) erf_ptr + erf_hdr_len;

		hdr.len = ntohs(r->wlen);
		hdr.caplen = ntohs(r->rlen) - erf_hdr_len;
		}
	while ( ! ApplyBPFFilter(current_filter, &hdr, data));

	++stats.received;

	// Timestamp conversion taken from DAG programming manual.
	unsigned long long lts = r->ts;
	hdr.ts.tv_sec = lts >> 32;
	lts = ((lts & 0xffffffffULL) * 1000 * 1000);
	lts += (lts & 0x80000000ULL) << 1;
	hdr.ts.tv_usec = lts >> 32;
	if ( hdr.ts.tv_usec >= 1000000 )
		{
		hdr.ts.tv_usec -= 1000000;
		hdr.ts.tv_sec += 1;
		}

	pkt->Init(props.link_type, &hdr.ts, hdr.caplen, hdr.len, data);

	//next_timestamp = hdr.ts.tv_sec + double(hdr.ts.tv_usec) / 1e6;

	return true;
	}

void PktDagSrc::DoneWithPacket()
	{
	// Nothing to do.
	}

// TODO: What is needed in modern Bro for this?
#if 0
void PktDagSrc::GetFds(int* read, int* write, int* except)
	{
	// We don't have a selectable fd, but we take the opportunity to
	// reset our idle flag if we have data now.
	if ( ! data )
		ExtractNextPacket();
	}
#endif

void PktDagSrc::Statistics(Stats* s)
	{
	s->received = stats.received;
	s->dropped = stats.dropped;
	s->link = stats.link + stats.dropped;
	}

bool PktDagSrc::PrecompileFilter(int index, const std::string& filter)
	{
	return PktSrc::PrecompileBPFFilter(index, filter);
	}

bool PktDagSrc::SetFilter(int index)
	{
	current_filter = index;

	// Reset counters.
	stats.received = stats.dropped = stats.link = 0;

	return true;
	}

iosource::PktSrc* PktDagSrc::InstantiatePktDagSrc(const std::string& path, bool is_live)
	{
	return new PktDagSrc(path, is_live);
	}
