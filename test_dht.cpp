#include <libtorrent/session.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/bdecode.hpp>
#include <iostream>
#include <vector>

int main() {
    libtorrent::session sess;
    libtorrent::entry e;
    sess.save_state(e, libtorrent::session_handle::save_dht_state);
    std::vector<char> buf;
    libtorrent::bencode(std::back_inserter(buf), e);
    
    libtorrent::bdecode_node node;
    libtorrent::error_code ec;
    libtorrent::bdecode(&buf[0], &buf[0] + buf.size(), node, ec);
    sess.load_state(node, libtorrent::session_handle::save_dht_state);
    return 0;
}
