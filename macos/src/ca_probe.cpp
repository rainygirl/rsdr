/* Inspect MPEG-2 TS conditional-access signalling and optionally dump ECM/EMM. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

struct SectionState {
	std::vector<uint8_t> data;
	size_t expected;
	int continuity;
	SectionState() : expected(0), continuity(-1) {}
};

struct CaDescriptor {
	int systemId;
	int pid;
	std::string source;
};

struct StreamInfo {
	int type;
	uint64_t clearPackets;
	uint64_t evenPackets;
	uint64_t oddPackets;
	StreamInfo() : type(-1), clearPackets(0), evenPackets(0), oddPackets(0) {}
};

struct MessageInfo {
	int pid;
	int tableId;
	std::vector<uint8_t> data;
};

static uint32_t
MpegCrc(const uint8_t* data, size_t size)
{
	uint32_t crc = 0xffffffffU;
	for (size_t i = 0; i < size; i++) {
		crc ^= (uint32_t)data[i] << 24;
		for (int bit = 0; bit < 8; bit++)
			crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04c11db7U : crc << 1;
	}
	return crc;
}

static uint64_t
Fnv64(const uint8_t* data, size_t size)
{
	uint64_t hash = 1469598103934665603ULL;
	for (size_t i = 0; i < size; i++) {
		hash ^= data[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

class Probe {
public:
	Probe() : packets(0), badSync(0), transportErrors(0), program(-1), pmtPid(-1) {}

	void Feed(const uint8_t* packet)
	{
		packets++;
		if (packet[0] != 0x47) {
			badSync++;
			return;
		}
		if (packet[1] & 0x80)
			transportErrors++;
		int pid = ((packet[1] & 0x1f) << 8) | packet[2];
		int scrambling = packet[3] >> 6;
		std::map<int, StreamInfo>::iterator stream = streams.find(pid);
		if (stream != streams.end()) {
			if (scrambling == 2)
				stream->second.evenPackets++;
			else if (scrambling == 3)
				stream->second.oddPackets++;
			else
				stream->second.clearPackets++;
		}

		bool interesting = pid == 0 || pid == 1 || pid == pmtPid
			|| caPids.find(pid) != caPids.end();
		if (!interesting)
			return;
		int afc = (packet[3] >> 4) & 3;
		if (afc != 1 && afc != 3)
			return;
		size_t pos = 4;
		if (afc == 3) {
			pos += 1 + packet[4];
			if (pos >= 188)
				return;
		}
		SectionState& state = sections[pid];
		int cc = packet[3] & 0x0f;
		if (state.continuity >= 0 && cc != ((state.continuity + 1) & 0x0f))
			ResetSection(state);
		state.continuity = cc;
		bool start = (packet[1] & 0x40) != 0;
		if (start) {
			size_t pointer = packet[pos++];
			if (pos + pointer > 188) {
				ResetSection(state);
				return;
			}
			if (!state.data.empty() && pointer != 0)
				Append(pid, state, packet + pos, pointer);
			pos += pointer;
			ResetSection(state);
		}
		if (pos < 188)
			Append(pid, state, packet + pos, 188 - pos);
	}

	void Print(const std::string& dumpPrefix)
	{
		printf("packets: %llu, bad sync: %llu, transport errors: %llu\n",
			(unsigned long long)packets, (unsigned long long)badSync,
			(unsigned long long)transportErrors);
		printf("program: %d, PMT PID: 0x%04X\n", program, pmtPid);
		for (size_t i = 0; i < descriptors.size(); i++) {
			printf("CA: system 0x%04X, PID 0x%04X (%s)\n",
				descriptors[i].systemId, descriptors[i].pid,
				descriptors[i].source.c_str());
		}
		for (std::map<int, StreamInfo>::const_iterator it = streams.begin();
			it != streams.end(); ++it) {
			printf("stream: PID 0x%04X, type 0x%02X, clear %llu, even %llu, odd %llu\n",
				it->first, it->second.type,
				(unsigned long long)it->second.clearPackets,
				(unsigned long long)it->second.evenPackets,
				(unsigned long long)it->second.oddPackets);
		}

		std::set<uint64_t> unique;
		int dumpIndex = 0;
		for (size_t i = 0; i < messages.size(); i++) {
			uint64_t hash = Fnv64(&messages[i].data[0], messages[i].data.size());
			if (!unique.insert(hash).second)
				continue;
			int count = 0;
			for (size_t j = 0; j < messages.size(); j++) {
				if (messages[j].pid == messages[i].pid
					&& messages[j].data == messages[i].data)
					count++;
			}
			bool crcOk = MpegCrc(&messages[i].data[0], messages[i].data.size()) == 0;
			printf("CA message: PID 0x%04X, table 0x%02X, %lu bytes, repeated %d, "
				"hash %016llX, CRC %s\n", messages[i].pid, messages[i].tableId,
				(unsigned long)messages[i].data.size(), count,
				(unsigned long long)hash, crcOk ? "ok" : "bad/private");
			if (!dumpPrefix.empty()) {
				char path[1024];
				snprintf(path, sizeof(path), "%s-pid%04x-table%02x-%03d.bin",
					dumpPrefix.c_str(), messages[i].pid, messages[i].tableId,
					dumpIndex++);
				std::ofstream out(path, std::ios::binary);
				out.write((const char*)&messages[i].data[0], messages[i].data.size());
				printf("  wrote %s\n", path);
			}
		}
		printf("CA messages: %lu total, %lu unique\n",
			(unsigned long)messages.size(), (unsigned long)unique.size());
	}

private:
	static void ResetSection(SectionState& state)
	{
		state.data.clear();
		state.expected = 0;
	}

	void Append(int pid, SectionState& state, const uint8_t* data, size_t size)
	{
		while (size != 0) {
			if (state.data.empty() && data[0] == 0xff)
				return;
			if (state.expected == 0 && state.data.size() < 3) {
				size_t take = std::min(size, 3 - state.data.size());
				state.data.insert(state.data.end(), data, data + take);
				data += take;
				size -= take;
				if (state.data.size() == 3) {
					state.expected = 3 + (((state.data[1] & 0x0f) << 8)
						| state.data[2]);
					if (state.expected < 3 || state.expected > 4096) {
						ResetSection(state);
						return;
					}
				}
				continue;
			}
			size_t take = std::min(size, state.expected - state.data.size());
			state.data.insert(state.data.end(), data, data + take);
			data += take;
			size -= take;
			if (state.data.size() == state.expected) {
				OnSection(pid, state.data);
				ResetSection(state);
			}
		}
	}

	void AddDescriptors(const uint8_t* data, size_t size, const std::string& source)
	{
		for (size_t pos = 0; pos + 2 <= size;) {
			int tag = data[pos++];
			size_t length = data[pos++];
			if (pos + length > size)
				break;
			if (tag == 0x09 && length >= 4) {
				CaDescriptor d;
				d.systemId = (data[pos] << 8) | data[pos + 1];
				d.pid = ((data[pos + 2] & 0x1f) << 8) | data[pos + 3];
				d.source = source;
				bool duplicate = false;
				for (size_t i = 0; i < descriptors.size(); i++) {
					if (descriptors[i].systemId == d.systemId
						&& descriptors[i].pid == d.pid
						&& descriptors[i].source == d.source)
						duplicate = true;
				}
				if (!duplicate)
					descriptors.push_back(d);
				caPids.insert(d.pid);
			}
			pos += length;
		}
	}

	void OnSection(int pid, const std::vector<uint8_t>& data)
	{
		if (data.empty())
			return;
		int table = data[0];
		if (pid == 0 && table == 0x00 && data.size() >= 12) {
			for (size_t pos = 8; pos + 4 <= data.size() - 4; pos += 4) {
				int number = (data[pos] << 8) | data[pos + 1];
				if (number != 0) {
					program = number;
					pmtPid = ((data[pos + 2] & 0x1f) << 8) | data[pos + 3];
					break;
				}
			}
			return;
		}
		if (pid == 1 && table == 0x01 && data.size() >= 12) {
			AddDescriptors(&data[8], data.size() - 12, "CAT/EMM");
			return;
		}
		if (pid == pmtPid && table == 0x02 && data.size() >= 16) {
			size_t programInfo = ((data[10] & 0x0f) << 8) | data[11];
			if (12 + programInfo > data.size() - 4)
				return;
			AddDescriptors(&data[12], programInfo, "PMT/program");
			for (size_t pos = 12 + programInfo; pos + 5 <= data.size() - 4;) {
				int type = data[pos];
				int streamPid = ((data[pos + 1] & 0x1f) << 8) | data[pos + 2];
				size_t info = ((data[pos + 3] & 0x0f) << 8) | data[pos + 4];
				if (pos + 5 + info > data.size() - 4)
					break;
				streams[streamPid].type = type;
				char source[64];
				snprintf(source, sizeof(source), "PMT/PID 0x%04X", streamPid);
				AddDescriptors(&data[pos + 5], info, source);
				pos += 5 + info;
			}
			return;
		}
		if (caPids.find(pid) != caPids.end()) {
			MessageInfo message;
			message.pid = pid;
			message.tableId = table;
			message.data = data;
			messages.push_back(message);
		}
	}

public:
	uint64_t packets;
	uint64_t badSync;
	uint64_t transportErrors;
	int program;
	int pmtPid;
	std::map<int, SectionState> sections;
	std::vector<CaDescriptor> descriptors;
	std::set<int> caPids;
	std::map<int, StreamInfo> streams;
	std::vector<MessageInfo> messages;
};

int
main(int argc, char** argv)
{
	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s <input.ts> [dump-prefix]\n", argv[0]);
		return 2;
	}
	std::ifstream in(argv[1], std::ios::binary);
	if (!in) {
		perror(argv[1]);
		return 1;
	}
	Probe probe;
	uint8_t packet[188];
	while (in.read((char*)packet, sizeof(packet)))
		probe.Feed(packet);
	probe.Print(argc == 3 ? argv[2] : "");
	return probe.badSync == 0 ? 0 : 1;
}
