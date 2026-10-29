"""
DireBot Link over a real socket, with the very client library DireBot
uses (obsws-python). The server is the real one; only OBS is pretend
(tests/harness.cpp).

    tools/test-link.sh          builds the harness and runs these
"""

import json
import os
import socket
import subprocess
import threading
import time

import pytest

obsws = pytest.importorskip("obsws_python")
websocket = pytest.importorskip("websocket")

HARNESS = os.environ.get("DIREBOT_LINK_HARNESS", "")
PASSWORD = "a-long-random-link-password"

pytestmark = pytest.mark.skipif(not HARNESS or not os.path.exists(HARNESS),
                                reason="needs the harness (tools/test-link.sh)")


class Harness:
    def __init__(self, port=0):
        self.proc = subprocess.Popen(
            [HARNESS, PASSWORD, str(port)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            bufsize=1)
        line = self.proc.stdout.readline().split()
        assert line and line[0] == "PORT", line
        self.port = int(line[1])
        self.log = []
        threading.Thread(target=self._read_log, daemon=True).start()

    def _read_log(self):
        with self.proc.stderr:
            for line in self.proc.stderr:
                self.log.append(line.strip())

    def say(self, text):
        self.proc.stdin.write(text + "\n")
        self.proc.stdin.flush()

    def ask(self, text, prefix):
        self.say(text)
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise AssertionError("the harness stopped")
            if line.startswith(prefix):
                return line.split(None, 1)[1].strip()

    def quit(self):
        try:
            if self.proc.poll() is None:
                self.say("quit")
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()
            self.proc.wait()
        for pipe in (self.proc.stdin, self.proc.stdout):
            try:
                pipe.close()
            except OSError:
                pass


@pytest.fixture
def link():
    h = Harness()
    yield h
    h.quit()


def wait_for(test, seconds=3.0):
    until = time.time() + seconds
    while time.time() < until:
        if test():
            return True
        time.sleep(0.02)
    return test()


def req(link, **kw):
    return obsws.ReqClient(host="127.0.0.1", port=link.port,
                           password=kw.pop("password", PASSWORD), timeout=3,
                           **kw)


def test_requests_through_obsws_python(link):
    client = req(link)
    try:
        assert client.get_version().obs_version == "32.2.2"
        scenes = client.get_scene_list().scenes
        assert [s["sceneName"] for s in scenes] == ["BRB", "Live"]
        assert client.get_current_program_scene() \
            .current_program_scene_name == "Live"
        echo = client.send("EchoTest", {"list": [1, "two", None]}, raw=True)
        assert echo == {"list": [1, "two", None]}
        with pytest.raises(obsws.error.OBSSDKRequestError) as e:
            client.send("NoSuchRequest")
        assert e.value.code == 204
    finally:
        client.disconnect()


def test_wrong_password_is_turned_away(link):
    with pytest.raises(Exception):
        req(link, password="guess")
    assert wait_for(lambda: any("password was wrong" in l for l in link.log))
    # ...and the link carries on for the right one.
    client = req(link)
    assert client.get_version().obs_version == "32.2.2"
    client.disconnect()


def test_no_password_is_turned_away(link):
    with pytest.raises(Exception):
        req(link, password="")


def test_scene_events_reach_the_scene_feed_only(link):
    got, meters = [], []

    def on_current_program_scene_changed(data):
        got.append(data.scene_name)

    def on_input_volume_meters(data):
        meters.append(data)

    events = obsws.EventClient(host="127.0.0.1", port=link.port,
                               password=PASSWORD, subs=obsws.Subs.SCENES,
                               timeout=3)
    events.callback.register([on_current_program_scene_changed,
                              on_input_volume_meters])
    requests = req(link)
    try:
        assert wait_for(lambda: link.ask("wanted", "WANTED") == "4")
        link.say('event 4 CurrentProgramSceneChanged {"sceneName":"BRB",'
                 '"sceneUuid":"x"}')
        link.say("meters 3")
        # An Inputs event: the scene feed didn't ask for it.
        link.say('event 8 InputMuteStateChanged {"inputName":"Mic",'
                 '"inputMuted":true}')
        assert wait_for(lambda: got == ["BRB"])
        time.sleep(0.2)
        assert meters == []
        # The request connection asked for no events: still answers right.
        assert requests.get_version().obs_version == "32.2.2"
    finally:
        events.disconnect()
        requests.disconnect()


def test_meters_reach_the_meter_feed(link):
    levels = []

    def on_input_volume_meters(data):
        levels.extend(row["inputLevelsMul"] for row in data.inputs)

    events = obsws.EventClient(host="127.0.0.1", port=link.port,
                               password=PASSWORD,
                               subs=obsws.Subs.INPUTVOLUMEMETERS, timeout=3)
    events.callback.register(on_input_volume_meters)
    try:
        assert wait_for(lambda: link.ask("wanted", "WANTED") == str(1 << 16))
        link.say("meters 5")
        assert wait_for(lambda: len(levels) == 5)
        assert levels[0] == [[0.5, 0.7, 0.8]]
    finally:
        events.disconnect()
    # Nobody listening: the meters can stop.
    assert wait_for(lambda: link.ask("wanted", "WANTED") == "0")


def raw(link, origin=None, host=None):
    headers = []
    if host:
        headers.append("Host: " + host)
    kw = {"timeout": 3}
    if origin:
        kw["origin"] = origin
    if headers:
        kw["header"] = headers
    return websocket.create_connection(f"ws://127.0.0.1:{link.port}", **kw)


def login(ws):
    hello = json.loads(ws.recv())
    auth = hello["d"]["authentication"]
    import base64
    import hashlib
    secret = base64.b64encode(hashlib.sha256(
        (PASSWORD + auth["salt"]).encode()).digest())
    answer = base64.b64encode(hashlib.sha256(
        secret + auth["challenge"].encode()).digest()).decode()
    ws.send(json.dumps({"op": 1, "d": {"rpcVersion": 1,
                                       "authentication": answer,
                                       "eventSubscriptions": 0}}))
    assert json.loads(ws.recv())["op"] == 2


def test_a_web_page_is_turned_away(link):
    with pytest.raises(websocket.WebSocketBadStatusException) as e:
        raw(link, origin="https://some-website.example")
    assert e.value.status_code == 403
    with pytest.raises(websocket.WebSocketBadStatusException):
        raw(link, host="evil.example")
    # DireBot's own client sends its own address as the origin: fine.
    ws = raw(link, origin=f"http://127.0.0.1:{link.port}")
    login(ws)
    ws.close()


def test_answers_come_back_in_order(link):
    ws = raw(link)
    login(ws)
    ws.send(json.dumps({"op": 6, "d": {"requestType": "SlowTest",
                                       "requestId": "1"}}))
    ws.send(json.dumps({"op": 6, "d": {"requestType": "EchoTest",
                                       "requestId": "2",
                                       "requestData": {"n": 2}}}))
    first, second = json.loads(ws.recv()), json.loads(ws.recv())
    assert first["d"]["requestId"] == "1"
    assert second["d"]["requestId"] == "2"
    assert second["d"]["responseData"] == {"n": 2}
    ws.ping("hi")
    ws.send(json.dumps({"op": 8, "d": {"requestId": "b", "requests": [
        {"requestType": "GetVersion"}, {"requestType": "EchoTest",
                                        "requestData": {"x": 1}}]}}))
    batch = json.loads(ws.recv())
    assert batch["op"] == 9
    assert [r["requestStatus"]["code"] for r in batch["d"]["results"]] == \
        [100, 100]
    ws.close()


def closed_with(ws):
    opcode, data = ws.recv_data(control_frame=True)
    assert opcode == websocket.ABNF.OPCODE_CLOSE
    return int.from_bytes(data[:2], "big")


def test_bad_messages_close_with_obs_websocket_codes(link):
    ws = raw(link)
    ws.recv()                      # hello
    # A request before logging in.
    ws.send(json.dumps({"op": 6, "d": {"requestType": "GetVersion",
                                       "requestId": "1"}}))
    assert closed_with(ws) == 4007
    ws2 = raw(link)
    ws2.recv()
    ws2.send("{not json")
    assert closed_with(ws2) == 4002


def test_many_clients_come_and_go(link):
    clients = [req(link) for _ in range(6)]
    assert link.ask("clients", "CLIENTS") == "6"
    for c in clients:
        c.disconnect()
    assert wait_for(lambda: link.ask("clients", "CLIENTS") == "0")


def test_closing_obs_says_goodbye(link):
    ws = raw(link)
    login(ws)
    link.say("quit")
    assert closed_with(ws) == 1001
    link.proc.wait(timeout=5)
    assert link.proc.stdout.read().strip().endswith("STOPPED")


def test_the_port_asked_for_and_another_when_its_taken():
    taken = socket.socket()
    taken.bind(("127.0.0.1", 0))
    taken.listen(1)
    port = taken.getsockname()[1]
    try:
        h = Harness(port)
        try:
            assert h.port != port and h.port > 0
            c = req(h)
            assert c.get_version().obs_version == "32.2.2"
            c.disconnect()
        finally:
            h.quit()
    finally:
        taken.close()
    h = Harness(port)
    try:
        assert h.port == port
    finally:
        h.quit()


def test_only_this_pc_can_reach_it(link):
    # Listening on 127.0.0.1 only: the PC's other addresses don't answer.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        mine = s.getsockname()[0]
    except OSError:
        mine = ""
    finally:
        s.close()
    if not mine or mine.startswith("127."):
        pytest.skip("this machine has no other address")
    with pytest.raises(OSError):
        socket.create_connection((mine, link.port), timeout=1)
