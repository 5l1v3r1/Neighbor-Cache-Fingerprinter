#include <iostream>
#include <dumbnet.h>
#include <cstring>
#include <sstream>
#include <pthread.h>

#include "InterfacePacketCapture.h"
#include "Fingerprinter.h"
#include "ArpFingerprint.h"
#include "Config.h"
#include "Probes.h"
#include "helpers.h"
#include "Lock.h"

using namespace std;
using namespace Nova;


// TOOD: Might want to move state data out of the fingerprint and into per test structs of some sort
pthread_mutex_t cbLock;
ResponseBehavior response;
ArpFingerprint fingerprint;
bool seenProbe = false;
bool replyToArp = false;

addr broadcastMAC, zeroIP, zeroMAC, origSrcMac;

Prober prober;

timeval lastARPReply; /* Used to compute the time between ARP requests */

void packetCallback(unsigned char *index, const struct pcap_pkthdr *pkthdr, const unsigned char *packet) {
	Lock lock(&cbLock);

	if (pkthdr->len < ETH_HDR_LEN)
		return;

	eth_hdr *eth = (eth_hdr*)packet;
	addr dstMac, srcMac;

	addr_pack_eth(&dstMac, (uint8_t*)&eth->eth_dst);
	addr_pack_eth(&srcMac, (uint8_t*)&eth->eth_src);

	if (ntohs(eth->eth_type) == ETH_TYPE_ARP) {
		/* We ignore everything before our probe has been sent */
		if (!seenProbe)
			return;

		if (pkthdr->len < ETH_HDR_LEN + ARP_HDR_LEN)
			return;

		arp_hdr *arp = (arp_hdr*)(packet + ETH_HDR_LEN);
		if (ntohs(arp->ar_op) == ARP_OP_REQUEST) {
			if (pkthdr->len < ETH_HDR_LEN + ARP_HDR_LEN + ARP_ETHIP_LEN)
				return;

			arp_ethip *arpRequest = (arp_ethip*)(packet + ETH_HDR_LEN + ARP_HDR_LEN);
			addr addr;
			addr_pack_ip(&addr, arpRequest->ar_tpa);


			if (addr_cmp(&addr, &CI->m_srcip) == 0) {
				cout << "Got an ARP request to " << addr_ntoa(&dstMac) << " for IP " << addr_ntoa(&addr) << " from " << addr_ntoa(&srcMac) << endl;

				if (addr_cmp(&dstMac, &CI->m_srcmac) == 0) {
					response.unicastUpdate = true;
				} else if (addr_cmp(&dstMac, &broadcastMAC) == 0) {
					response.unicastUpdate = false;
				} else {
					cout << "WARNING: Got an ARP packet that was neither to the broadcast MAC or our probe MAC. This is unusual." << endl;
				}

				response.requestAttempts++;

				if (replyToArp)
					prober.SendARPReply(&CI->m_srcmac, &CI->m_dstmac, &CI->m_srcip, &CI->m_dstip);

				int diff = 0;
				diff += 1000000*(pkthdr->ts.tv_sec  - lastARPReply.tv_sec);
				diff += pkthdr->ts.tv_usec - lastARPReply.tv_usec;
				cout << "Time since last ARP request was " << pkthdr->ts.tv_sec  - lastARPReply.tv_sec << " seconds " << endl;


				if (response.requestAttempts > 1 && response.requestAttempts < MAX_RECORDED_REPLIES) {
					response.timeBetweenRequests[response.requestAttempts - 2] = diff;

					/* Compute the average time between requests */
					double sum = 0;
					for (int i = 0; i < (response.requestAttempts - 1); i++)
						sum += response.timeBetweenRequests[i];
					response.averageTimeBetweenRequests = sum / (response.requestAttempts - 1);

					if (diff > response.m_maxTimebetweenRequests) {
						response.m_maxTimebetweenRequests = diff;
					}
					if (diff < response.m_minTimeBetweenRequests) {
						response.m_minTimeBetweenRequests = diff;
					}
				}
				lastARPReply = pkthdr->ts;
			}
		}
	} else if (ntohs(eth->eth_type) == ETH_TYPE_IP) {
		if (pkthdr->len < ETH_HDR_LEN + IP_HDR_LEN)
			return;

		ip_hdr *ip = (ip_hdr*)(packet + ETH_HDR_LEN);

		/* Check if our own probe packet went over the interface */
		if (!seenProbe) {
			Lock lock(&prober.probeBufferLock);
			if (pkthdr->len != prober.probeBufferSize)
				return;

			for (int i = 0; i < pkthdr->len; i++) {
				if (packet[i] != prober.probeBuffer[i])
					return;
			}

			seenProbe = true;
			//cout << "Packet capture thread has seen probe packet go out" << endl;
		} else {
			addr dstIp, srcIp;
			addr_pack_ip(&dstIp, (uint8_t*)&ip->ip_dst);
			addr_pack_ip(&srcIp, (uint8_t*)&ip->ip_src);


			if (addr_cmp(&dstIp, &CI->m_srcip) == 0) {
				if (addr_cmp(&dstMac, &CI->m_srcmac) == 0) {
					response.replyToCorrectMAC = true;
				} else {
					response.replyToCorrectMAC = false;
				}

				cout << "Saw a probe response to " << addr_ntoa(&dstIp) << " / " << addr_ntoa(&dstMac) << " from " << addr_ntoa(&srcIp) << " / " << addr_ntoa(&srcMac) << endl;
				response.sawProbeReply = true;
				if (response.requestAttempts == 0)
					response.replyBeforeARP = true;
				else
					response.replyBeforeARP = false;
			}

		}
	}
}


// This is used in the gratuitous ARP test for checking the result
bool gratuitousResultCheck() {
	bool result;

	prober.SendSYN(CI->m_dstip, CI->m_dstmac, CI->m_srcip, origSrcMac, CI->m_dstport, CI->m_srcport);
	usleep(1000000);

	pthread_mutex_lock(&cbLock);
	if (!response.sawProbeReply) {
		cout << "WARNING: Saw no probe response! Unable to perform test." << endl;
		//exit(1);
	}

	if (response.replyToCorrectMAC) {
		result = true;
		cout << "PASS: Gratuitous ARP was accepted into the table" << endl << endl;
	} else {
		result = false;
		cout << "FAIL: Gratuitous ARP was NOT accepted into the table" << endl << endl;;
	}

	response = ResponseBehavior();
	seenProbe = false;
	CI->m_srcmac.__addr_u.__eth.data[5]++;

	pthread_mutex_unlock(&cbLock);
	return result;
}

void checkInitialQueryBehavior()
{
	for (int i = 0; i < CI->m_retries; i++) {
		prober.SendSYN(CI->m_dstip, CI->m_dstmac, CI->m_srcip, CI->m_srcmac, CI->m_dstport, CI->m_srcport);
		sleep(CI->m_sleeptime);

		pthread_mutex_lock(&cbLock);
		cout << response.toString() << endl << endl;

		// TODO we save the requestAttempts from the first ARP. On android this changes
		// from 2 to 1 on the second retry, which we should add as a fingerprint feature
		if (i == 0)
		{
			fingerprint.requestAttempts = response.requestAttempts;
		}


		// Reset response if this isn't the last test
		if (i != CI->m_retries - 1) {
			// Save the min and max times
			ResponseBehavior f;
			f.m_maxTimebetweenRequests = response.m_maxTimebetweenRequests;
			f.m_minTimeBetweenRequests = response.m_minTimeBetweenRequests;
			response = f;
		}
		pthread_mutex_unlock(&cbLock);
	}


	// Populate our results into the fingerprint
	double difference = response.m_maxTimebetweenRequests - response.m_minTimeBetweenRequests;
	double percentDifference = 100*difference/response.m_minTimeBetweenRequests;
	cout << "Timing range difference of " << percentDifference << endl;

	if (percentDifference > 8) {
		fingerprint.constantRetryTime = false;
	} else {
		fingerprint.constantRetryTime = true;
	}
}

void checkStaleTiming() {
	// TODO: What do we do about the max for this? Could take 20 mins on freebsd
	// For now we just go up to a max of 1 min?
	int i;
	for (i = 0; i < 60; i++) {
		pthread_mutex_lock(&cbLock);
		response = ResponseBehavior();
		seenProbe = false;

		// Only reply to the 1st ARP request in this test
		if (i == 0)
			replyToArp = true;
		else
			replyToArp = false;
		pthread_mutex_unlock(&cbLock);

		prober.SendSYN(CI->m_dstip, CI->m_dstmac, CI->m_srcip, CI->m_srcmac, CI->m_dstport, CI->m_srcport);

		sleep(1);

		pthread_mutex_lock(&cbLock);
		cout << response.toString() << endl << endl;
		if (response.requestAttempts > 0 && i != 0) {
			break;
		}
		pthread_mutex_unlock(&cbLock);
	}

	fingerprint.referencedStaleTimeout = i;
	fingerprint.replyBeforeUpdate = response.sawProbeReply;
	fingerprint.unicastUpdate = response.unicastUpdate;

	pthread_mutex_unlock(&cbLock);
}

void checkGratuitousBehavior() {
	origSrcMac = CI->m_srcmac;

	pthread_mutex_lock(&cbLock);
	replyToArp = true;
	pthread_mutex_unlock(&cbLock);


	// Get ourselves into the ARP table
	prober.SendSYN(CI->m_dstip, CI->m_dstmac, CI->m_srcip, CI->m_srcmac, CI->m_dstport, CI->m_srcport);

	sleep(5);

	pthread_mutex_lock(&cbLock);
	response = ResponseBehavior();
	seenProbe = false;
	replyToArp = false;
	CI->m_srcmac.__addr_u.__eth.data[5]++;
	pthread_mutex_unlock(&cbLock);

	int probeTestNumber = 0;
	bool results[36];

	stringstream result;
	// Try for both ARP request and ARP reply opcodes
	for (int arpOpCode = 2; arpOpCode > 0; arpOpCode--) {
		for (int macDestination = 0; macDestination < 2; macDestination++) {
			for (int tpa = 0; tpa < 3; tpa++) {
				for (int tha = 0; tha < 3; tha++){
					addr tpaAddress;
					if (tpa == 0) {
						tpaAddress = zeroIP;
					} else if (tpa == 1) {
						tpaAddress = CI->m_srcip;
					} else if (tpa == 2) {
						tpaAddress = CI->m_dstip;
					}

					addr thaAddress;
					if (tha == 0) {
						thaAddress = zeroMAC;
					} else if (tha == 1) {
						thaAddress = broadcastMAC;
					} else if (tha == 2) {
						thaAddress = CI->m_dstmac;
					}


					// Ethernet frame destination MAC
					addr destinationMac;
					if (macDestination == 0) {
						destinationMac = broadcastMAC;
					} else if (macDestination == 1) {
						destinationMac = CI->m_dstmac;
					}

					prober.SendARPReply(&CI->m_srcmac, &destinationMac, &CI->m_srcip, &tpaAddress, arpOpCode, &thaAddress);
					usleep(1000000);

					bool testResult = gratuitousResultCheck();
					result << testResult;

					if (probeTestNumber >= 36) {
						cout << "ERROR: Invalid gratuitous probe number!" << endl;
						exit(1);
					}

					results[probeTestNumber] = testResult;
					probeTestNumber++;


					// Helps reset the neighbor cache's entry state to reachable for each test
					if (true || probeTestNumber % 10 == 0)
					{
						prober.SendARPReply(&origSrcMac, &CI->m_dstmac, &CI->m_srcip, &CI->m_dstip);
						sleep(3);
						pthread_mutex_lock(&cbLock);
						response = ResponseBehavior();
						seenProbe = false;
						pthread_mutex_unlock(&cbLock);

					}
				}
			}
		}
	}

	for (int i = 0; i < 36; i++) {
		fingerprint.gratuitousUpdates[i] = results[i];
	}

	cout << "Result fingerprint from gratuitous test," << endl;
	cout << result.str() << endl;
}

int main(int argc, char ** argv)
{
	Config::Inst()->LoadArgs(argv, argc);

	// Load the fingerprints
	Fingerprinter fingerprinter;
	fingerprinter.LoadFingerprints();

	/* Stuff the broadcast MAC in an addr type for comparison later */
	unsigned char broadcastBuffer[ETH_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	addr_pack_eth(&broadcastMAC, (uint8_t*)broadcastBuffer);

	// Stuff the zero value into an ip addr
	uint32_t zeroNumber = 0;
	addr_pack_ip(&zeroIP, (uint8_t*)&zeroNumber);

	// Stuff the zerio value into a MAC addr
	uint8_t zeroMacNumber[6];
	for (int i = 0; i < 6; i++)
		zeroMacNumber[i] = 0;
	addr_pack_eth(&zeroMAC, &zeroMacNumber[0]);


	stringstream pcapFilterString;
	pcapFilterString << "arp or (dst host " << CI->m_srcipString << ")";

	pthread_mutex_init(&cbLock, NULL);

	
	InterfacePacketCapture *capture = new InterfacePacketCapture(CI->m_interface);
	capture->Init();
	capture->SetFilter(pcapFilterString.str());
	capture->SetPacketCb(&packetCallback);
	capture->StartCapture();
	sleep(1);


	// This one doesn't update ARP tables on Linux 2.6 but seems to work in Linux 3.x.
	// The rest all work to update the table but not to create new entry in Linux.
	if (CI->m_test == 100) {
		prober.SendARPReply(&CI->m_srcmac, &broadcastMAC, &CI->m_srcip, &CI->m_srcip);
		return 0;
	}

	if (CI->m_test == 101) {
		prober.SendARPReply(&CI->m_srcmac, &broadcastMAC, &CI->m_srcip, (addr*)&zeroIP);
		return 0;
	}

	// This one adds an entry to the ARP table in FreeBSD
	if (CI->m_test == 102) {
		prober.SendARPReply(&CI->m_srcmac, &CI->m_dstmac, &CI->m_srcip, &CI->m_dstip);
		return 0;
	}

	if (CI->m_test == 103) {
		prober.SendARPReply(&CI->m_srcmac, &CI->m_dstmac, &CI->m_srcip, (addr*)&zeroIP);
		return 0;
	}

	if (CI->m_test == 200) {
		prober.SendSYN(CI->m_dstip, CI->m_dstmac, CI->m_srcip, CI->m_srcmac, CI->m_dstport, CI->m_srcport);
		return 0;
	}


	if (CI->m_test == 0) {
		checkInitialQueryBehavior();
		checkStaleTiming();
		sleep(3);
		checkGratuitousBehavior();

		cout << "FINGERPRINT FOLLOWS" << endl;
		cout << fingerprint.toString() << endl << endl;
		cout << fingerprint.toTinyString() << endl;

		cout << fingerprinter.GetMatchReport(fingerprint) << endl;
	}

	if (CI->m_test == 1) {
		checkInitialQueryBehavior();
	}

	if (CI->m_test == 2) {
		checkStaleTiming();
	}

	if (CI->m_test == 3) {
		/*
		 * We run this test twice to note a neat difference between Windows and Linux.
		 * In Linux, the first probe packet will cause the SYN/RST to put an entry in the ARP table, which will be
		 * set to FAIL state and then updated to STALE when it sees the gratuitous ARP, causing the 2nd probe to
		 * be replied to followed by ARP requests. Windows 7 at least will ignore the gratuitous ARP packet
		 * entirely and not exhibit the same behavior.
		*/
		for (int i = 0; i < 2; i++) {
			pthread_mutex_lock(&cbLock);
			response = ResponseBehavior();
			seenProbe = false;
			pthread_mutex_unlock(&cbLock);


			// Send gratuitous ARP reply
			addr zero;
			uint32_t zeroIp = 0;
			addr_pack_ip(&zero, (uint8_t*)&zeroIp);
			prober.SendARPReply(&CI->m_srcmac, &broadcastMAC, &CI->m_srcip, &CI->m_srcip);


			prober.SendSYN(CI->m_dstip, CI->m_dstmac, CI->m_srcip, CI->m_srcmac, CI->m_dstport, CI->m_srcport);
			sleep(CI->m_sleeptime);
			pthread_mutex_lock(&cbLock);
			cout << response.toString() << endl << endl;
			pthread_mutex_unlock(&cbLock);

		}
	}

	if (CI->m_test == 4) {
		checkGratuitousBehavior();
	}


	return 0;
}

