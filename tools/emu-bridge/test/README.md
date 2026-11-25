Bridge test without an emulator: a fake mGBA memory holds a mailbox, two
processes play two players through a real relay (UDP and ws://).

    gcc -w -I<mgba>/include -I<mgba>/src harness.c ../owl_bridge.c -lpthread -o harness
    python3 relay.py --port 19000 &  python3 relay_ws.py --port 19001 --relay 127.0.0.1:19000 &
    # a/owl.txt: host=127.0.0.1 port=19000 room=77 peer=1   (b: peer=2)
    ./harness a 11 & ./harness b 22      # each must print "saw other player's: YES"

TLS (wss://) is not covered here.
