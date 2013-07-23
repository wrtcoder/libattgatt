#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cassert>
#include <tuple>
#include <stdexcept>

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

#include <sys/socket.h>

#include <unistd.h>
#include "logging.h"
#include "lib/uuid.h"
#include "att_pdu.h"
using namespace std;

#define LE_ATT_CID 4        //Spec 4.0 G.5.2.2
#define ATT_DEFAULT_MTU 23  //Spec 4.0 G.5.2.1

#define GATT_CHARACTERISTIC 0x2803
#define GATT_CHARACTERISTIC_FLAGS_BROADCAST     0x01
#define GATT_CHARACTERISTIC_FLAGS_READ          0x02
#define GATT_CHARACTERISTIC_FLAGS_WRITE_WITHOUT_RESPONSE 0x04
#define GATT_CHARACTERISTIC_FLAGS_WRITE         0x08
#define GATT_CHARACTERISTIC_FLAGS_NOTIFY        0x10
#define GATT_CHARACTERISTIC_FLAGS_INDICATE      0x20
#define GATT_CHARACTERISTIC_FLAGS_AUTHENTICATED_SIGNED_WRITES 0x40
#define GATT_CHARACTERISTIC_FLAGS_EXTENDED_PROPERTIES      0x80

void test_fd_(int fd, int line)
{
	if(fd < 0)
	{
		cerr << "Error on line " << line << ": " <<strerror(errno) << endl;
		exit(1);
	}

	cerr << "Line " << line << " ok = " << fd  << endl;

}

string to_hex(const uint16_t& u)
{
	ostringstream os;
	os << setw(4) << setfill('0') << hex << u;
	return os.str();
}

string to_hex(const uint8_t& u)
{
	ostringstream os;
	os << setw(2) << setfill('0') << hex << (int)u;
	return os.str();
}

string to_str(const uint8_t& u)
{
	if(u < 32 || u > 126)
		return "\\x" + to_hex(u);
	else
	{
		char buf[] = {(char)u, 0};
		return buf;
	}
}

string to_str(const bt_uuid_t& uuid)
{
	//8 4 4 4 12
	if(uuid.type == BT_UUID16)
		return to_hex(uuid.value.u16);
	else if(uuid.type == BT_UUID128)
		return "--128--";
	else
		return "uuid.wtf";

}

string to_hex(const uint8_t* d, int l)
{
	ostringstream os;
	for(int i=0; i < l; i++)
		os << to_hex(d[i]) << " ";
	return os.str();
}
string to_hex(pair<const uint8_t*, int> d)
{
	return to_hex(d.first, d.second);
}

string to_hex(const vector<uint8_t>& v)
{
	return to_hex(v.data(), v.size());
}

string to_str(const uint8_t* d, int l)
{
	ostringstream os;
	for(int i=0; i < l; i++)
		os << to_str(d[i]);
	return os.str();
}
string to_str(pair<const uint8_t*, int> d)
{
	return to_str(d.first, d.second);
}
string to_str(pair<const uint8_t*, const uint8_t*> d)
{
	return to_str(d.first, d.second - d.first);
}

string to_str(const vector<uint8_t>& v)
{
	return to_str(v.data(), v.size());
}


#define test(X) test_fd_(X, __LINE__)


void pretty_print(const PDUResponse& pdu)
{
	if(log_level >= Debug)
	{
		cerr << "debug: ---PDU packet ---\n";
		cerr << "debug: " << to_hex(pdu.data, pdu.length) << endl;
		cerr << "debug: " << to_str(pdu.data, pdu.length) << endl;
		cerr << "debug: Packet type: " << to_hex(pdu.type()) << " " << att_op2str(pdu.type()) << endl;
		
		if(pdu.type() == ATT_OP_ERROR)
			cerr << "debug: " << PDUErrorResponse(pdu).error_str() << " in response to " <<  att_op2str(PDUErrorResponse(pdu).request_opcode()) << " on handle " + to_hex(PDUErrorResponse(pdu).handle()) << endl;
		else if(pdu.type() == ATT_OP_READ_BY_TYPE_RESP)
		{
			PDUReadByTypeResponse p(pdu);

			cerr << "debug: elements = " << p.num_elements() << endl;
			cerr << "debug: value size = " << p.value_size() << endl;

			for(int i=0; i < p.num_elements(); i++)
			{
				cerr << "debug: " << to_hex(p.handle(i)) << " ";
				if(p.value_size() != 2)
					cerr << "-->" << to_str(p.value(i)) << "<--" << endl;
				else
					cerr << to_hex(p.value_uint16(i)) << endl;
			}

		}
		else if(pdu.type() == ATT_OP_READ_BY_GROUP_RESP)
		{
			PDUReadGroupByTypeResponse p(pdu);
			cerr << "debug: elements = " << p.num_elements() << endl;
			cerr << "debug: value size = " << p.value_size() << endl;

			for(int i=0; i < p.num_elements(); i++)
				cerr << "debug: " <<  "[ " << to_hex(p.start_handle(i)) << ", " << to_hex(p.end_handle(i)) << ") :" << to_str(p.uuid(i)) << endl;
		}
		else
			cerr << "debug: --no pretty printer available--\n";
		
		cerr << "debug:\n";
	}
};




//Almost zero resource to represent the ATT protocol on a BLE
//device. This class does none of its own memory management, and will not generally allocate
//or do other nasty things. Oh no, it allocates a buffer!
//
//Mostly what it can do is write ATT command packets (PDUs) and receive PDUs back.
struct BLEDevice
{
	bool constructing;
	int sock;
	static const int buflen=ATT_DEFAULT_MTU;

	void test_fd_(int fd, int line)
	{
		if(fd < 0)
		{
			LOG(Info, "Error on line " << line << ": " <<strerror(errno));

			if(constructing && sock >= 0)
			{
				int ret = close(sock);
				if(ret < 0)
					LOG(Warning, "Error on line " << line << ": " <<strerror(errno));
				else
					LOG(Debug, "System call on " << line << ": " << strerror(errno));
			}
			exit(1);
		}
		else
			LOG(Debug, "System call on " << line << ": " << strerror(errno) << " ret = " << fd);
	}

	void test_pdu(int len)
	{
		if(len == 0)
			throw logic_error("Error constructing packet");
	}

	BLEDevice();

	void send_read_by_type(const bt_uuid_t& uuid, uint16_t start = 0x0001, uint16_t end=0xffff)
	{
		vector<uint8_t> buf(buflen);
		int len = enc_read_by_type_req(start, end, const_cast<bt_uuid_t*>(&uuid), buf.data(), buf.size());
		test_pdu(len);
		int ret = write(sock, buf.data(), len);
		test(ret);
	}

	void send_find_information(uint16_t start = 0x0001, uint16_t end=0xffff)
	{
		vector<uint8_t> buf(buflen);
		int len = enc_find_info_req(start, end, buf.data(), buf.size());
		test_pdu(len);
		int ret = write(sock, buf.data(), len);
		test(ret);
	}

	void send_read_group_by_type(const bt_uuid_t& uuid, uint16_t start = 0x0001, uint16_t end=0xffff)
	{
		vector<uint8_t> buf(buflen);
		int len = enc_read_by_grp_req(start, end, const_cast<bt_uuid_t*>(&uuid), buf.data(), buf.size());
		test_pdu(len);
		int ret = write(sock, buf.data(), len);
		test(ret);
	}

	PDUResponse receive(uint8_t* buf, int max)
	{
		int len = read(sock, buf, max);
		test(len);
		pretty_print(PDUResponse(buf, len));
		return PDUResponse(buf, len);
	}

	PDUResponse receive(vector<uint8_t>& v)
	{
		return receive(v.data(), v.size());
	}
};

//Easier to use implementation of the ATT protocol.
//Blocks, rather than chunking packets.
struct SimpleBlockingATTDevice: public BLEDevice
{
	template<class Ret, class PDUType, class E, class F, class G> 
	vector<Ret> read_multiple(int request, int response, const E& call,  const F& func, const G& last)
	{
		vector<Ret> ret;
		vector<uint8_t> buf(ATT_DEFAULT_MTU);
		
		int start=1;

		for(;;)
		{
			call(start, 0xffff);
			PDUResponse r = receive(buf);
		
			if(r.type() == ATT_OP_ERROR)
			{
				PDUErrorResponse err = r;

				if(err.request_opcode() != request)
				{
					LOG(Error, string("Unexpected opcode in error. Expected ") + att_op2str(request) + " got "  + att_op2str(err.request_opcode()));
					throw 1;
				}
				else if(err.error_code() != ATT_ECODE_ATTR_NOT_FOUND)
				{
					LOG(Error, string("Received unexpected error:") + att_ecode2str(err.error_code()));
					throw 1;
				}
				else 
					break;
			}
			else if(r.type() != response)
			{
					LOG(Error, string("Unexpected response. Expected ") + att_op2str(response) + " got "  + att_op2str(r.type()));
			}
			else
			{
				PDUType t = r;
				for(int i=0; i < t.num_elements(); i++)
					ret.push_back(func(t, i));
				
				if(last(t) == 0xffff)
					break;
				else
					start = last(t)+1;

				LOG(Debug, "New start = " << start);
			}
		}

		return ret;

	}


	vector<pair<uint16_t, vector<uint8_t>>> read_by_type(const bt_uuid_t& uuid)
	{
		return read_multiple<pair<uint16_t, vector<uint8_t>>, PDUReadByTypeResponse>(ATT_OP_READ_BY_TYPE_REQ, ATT_OP_READ_BY_TYPE_RESP, 
			[&](int start, int end)
			{
				send_read_by_type(uuid, start, end);	
			},
			[](const PDUReadByTypeResponse& p, int i)
			{
				return make_pair(p.handle(i),  vector<uint8_t>(p.value(i).first, p.value(i).second));
			}, 
			[](const PDUReadByTypeResponse& p)
			{
				return p.handle(p.num_elements()-1);
			})
			;

	}

	vector<tuple<uint16_t, uint16_t, bt_uuid_t>> read_by_group_type(const bt_uuid_t& uuid)
	{
		return read_multiple<tuple<uint16_t, uint16_t, bt_uuid_t>, PDUReadGroupByTypeResponse>(ATT_OP_READ_BY_GROUP_REQ, ATT_OP_READ_BY_GROUP_RESP, 
			[&](int start, int end)
			{
				send_read_group_by_type(uuid, start, end);	
			},
			[](const PDUReadGroupByTypeResponse& p, int i)
			{
				return make_tuple(p.start_handle(i),  p.end_handle(i), p.uuid(i));
			},
			[](const PDUReadGroupByTypeResponse& p)
			{
				return p.end_handle(p.num_elements()-1);
			});
	}

	
	vector<pair<uint16_t, bt_uuid_t>> find_information()
	{
		return read_multiple<pair<uint16_t, bt_uuid_t>, PDUFindInformationResponse>(ATT_OP_FIND_INFO_REQ, ATT_OP_FIND_INFO_RESP, 
			[&](int start, int end)
			{
				send_find_information(start, end);
			},
			[](const PDUFindInformationResponse&p, int i)
			{
				return make_pair(p.handle(i), p.uuid(i));
			},
			[](const PDUFindInformationResponse& p)
			{
				return p.handle(p.num_elements()-1);
			});
	}
};



class GATTReadCharacteristic: public  PDUReadByTypeResponse
{
	public:
	struct Characteristic
	{
		uint16_t handle;
		uint8_t  flags;
		bt_uuid_t uuid;
	};

	GATTReadCharacteristic(const PDUResponse& p)
	:PDUReadByTypeResponse(p)
	{
		if(value_size() != 5 && value_size() != 19)		
			throw runtime_error("Invalid packet size in GATTReadCharacteristic");
	}

	Characteristic characteristic(int i) const
	{
		Characteristic c;
		c.handle = att_get_u16(value(i).first + 1);
		c.flags  = value(i).first[0];

		if(value_size() == 5)
			c.uuid= att_get_uuid16(value(i).first + 3);
		else
			c.uuid= att_get_uuid128(value(i).first + 3);

		return c;
	}
};


class SimpleBlockingGATTDevice: public SimpleBlockingATTDevice
{
	public:

	typedef GATTReadCharacteristic::Characteristic Characteristic;

	vector<pair<uint16_t, Characteristic>> read_characaristic()
	{
		bt_uuid_t uuid;
		uuid.value.u16 = GATT_CHARACTERISTIC;
		uuid.type = BT_UUID16;
		return read_multiple<pair<uint16_t, Characteristic>, GATTReadCharacteristic>(ATT_OP_READ_BY_TYPE_REQ, ATT_OP_READ_BY_TYPE_RESP, 
			[&](int start, int end)
			{
				send_read_by_type(uuid, start, end);	
			},
			[](const GATTReadCharacteristic& p, int i)
			{
				return make_pair(p.handle(i), p.characteristic(i));
			},
			[](const GATTReadCharacteristic& p)
			{
				return p.handle(p.num_elements()-1);
			});
	}


};






template<class C> const C& haxx(const C& X)
{
	return X;
}

int haxx(uint8_t X)
{
	return X;
}
#define LOGVAR(X) LOG(Debug, #X << " = " << haxx(X))

BLEDevice::BLEDevice()
:constructing(true)
{
	//Allocate socket and create endpoint.
	sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	test(sock);
	
	////////////////////////////////////////
	//Bind the socket
	//I believe that l2 is for an l2cap socket. These are kind of like
	//UDP in that they have port numbers and are packet oriented.
	struct sockaddr_l2 addr;
		
	bdaddr_t source_address = {{0,0,0,0,0,0}};  //i.e. the adapter. Note, 0, corresponds to BDADDR_ANY
	                                //However BDADDR_ANY uses a nonstandard C hack and does not compile
									//under C++. So, set it manually.
	//So, a sockaddr_l2 has the family (obviously)
	//a PSM (wtf?) 
	//  Protocol Service Multiplexer (WTF?)
	//an address (of course)
	//a CID (wtf) 
	//  Channel ID (i.e. port number?)
	//and an address type (wtf)

	memset(&addr, 0, sizeof(addr));
	addr.l2_family = AF_BLUETOOTH;

	addr.l2_psm = 0;
	addr.l2_cid = htobs(LE_ATT_CID);


	bacpy(&addr.l2_bdaddr, &source_address);

	//Address type: Low Energy public
	addr.l2_bdaddr_type=BDADDR_LE_PUBLIC;
	
	//Bind socket. This associates it with a particular adapter.
	//We chose ANY as the source address, so packets will go out of 
	//whichever adapter necessary.
	int ret = bind(sock, (sockaddr*)&addr, sizeof(addr));
	test(ret);
	

	////////////////////////////////////////
	
	//Need to do l2cap_set here
	l2cap_options options;
	unsigned int len = sizeof(options);
	memset(&options, 0, len);

	//Get the options with a minor bit of cargo culting.
	//SOL_L2CAP seems to mean that is should operate at the L2CAP level of the stack
	//L2CAP_OPTIONS who knows?
	ret = getsockopt(sock, SOL_L2CAP, L2CAP_OPTIONS, &options, &len);
	test(ret);

	LOGVAR(options.omtu);
	LOGVAR(options.imtu);
	LOGVAR(options.flush_to);
	LOGVAR(options.mode);
	LOGVAR(options.fcs);
	LOGVAR(options.max_tx);
	LOGVAR(options.txwin_size);


	
	//Can also use bacpy to copy addresses about
	str2ba("3C:2D:B7:85:50:2A", &addr.l2_bdaddr);
	ret = connect(sock, (sockaddr*)&addr, sizeof(addr));
	test(ret);


	//And this seems to work up to here.

	//Get the options with a minor bit of cargo culting.
	//SOL_L2CAP seems to mean that is should operate at the L2CAP level of the stack
	//L2CAP_OPTIONS who knows?
	ret = getsockopt(sock, SOL_L2CAP, L2CAP_OPTIONS, &options, &len);
	test(ret);

	LOGVAR(options.omtu);
	LOGVAR(options.imtu);
	LOGVAR(options.flush_to);
	LOGVAR(options.mode);
	LOGVAR(options.fcs);
	LOGVAR(options.max_tx);
	LOGVAR(options.txwin_size);


}

LogLevels log_level;

int main(int , char **)
{
	log_level = Warning;
	vector<uint8_t> buf(256);

	SimpleBlockingGATTDevice b;
	
	bt_uuid_t uuid;
	uuid.type = BT_UUID16;

	uuid.value.u16 = 0x2800;
	
	
	//log_level=Trace;

	auto r = b.read_by_type(uuid);

	for(unsigned int i=0; i < r.size(); i++)
	{
		cout << "Handle: " << to_hex(r[i].first) << ", Data: " << to_hex(r[i].second) << endl;
		cout <<                "-->" << to_str(r[i].second) << "<--" << endl;
	}



	auto s = b.read_by_group_type(uuid);
		
	for(unsigned int i=0; i < s.size(); i++)
	{
		cout << "Start: " << to_hex(get<0>(s[i]));
		cout << " End: " << to_hex(get<1>(s[i]));
		cout << " UUID: " << to_str(get<2>(s[i])) << endl;
	}
	
	auto r1 = b.read_characaristic();
	for(const auto& i: r1)
	{
		cout << to_hex(i.first) << " Handle: " << to_hex(i.second.handle) << "  UUID: " << to_str(i.second.uuid)<<  " Flags: ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_BROADCAST)
			cout << "Broadcast ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_READ)
			cout << "Read ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_WRITE_WITHOUT_RESPONSE)
			cout << "Write (without response) ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_WRITE)
			cout << "Write ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_NOTIFY)
			cout << "Notify ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_INDICATE)
			cout << "Indicate ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_AUTHENTICATED_SIGNED_WRITES)
			cout << "Authenticated signed writes ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_EXTENDED_PROPERTIES)
			cout << "Extended properties ";
		cout << endl;
	}

	cout << "Information\n";
	auto r2 = b.find_information();
	for(const auto &i: r2)
	{
		cout << "Handle: " << to_hex(i.first) << "	Type: " << to_str(i.second) << endl;
	}

	/*b.send_read_by_type(uuid);
	b.receive(buf);

	uuid.value.u16 = 0x2901;
	b.send_read_by_type(uuid);
	b.receive(buf);

	uuid.value.u16 = 0x2803;
	b.send_read_by_type(uuid);
	b.receive(buf);

	b.send_read_by_type(uuid, 0x0008);
	b.receive(buf);
	b.send_read_by_type(uuid, 0x0010);
	b.receive(buf);
	b.send_read_by_type(uuid, 0x0021);
	b.receive(buf);


	cerr << endl<<endl<<endl;
*/

/*
	cerr << endl<<endl<<endl;
	cerr << endl<<endl<<endl;
	cerr << endl<<endl<<endl;
	uuid.value.u16 = 0x2800;
	b.send_read_by_type(uuid);
	b.receive(buf);
	b.send_read_group_by_type(uuid);
	b.receive(buf);
	b.send_read_group_by_type(uuid, 0x0013);
	b.receive(buf);
*/
}
