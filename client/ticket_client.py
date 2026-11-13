import json
import urllib.request

SERVER = "http://your-server:8080"


def get_ticket(card: str, device: str) -> str:
    req = urllib.request.Request(
        SERVER + "/v1/ticket",
        data=json.dumps({"card": card, "device": device}).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as r:
        body = json.loads(r.read())
    if "ticket" not in body:
        raise RuntimeError(body.get("error", "unknown"))
    return body["ticket"]


if __name__ == "__main__":
    import sys
    print(get_ticket(sys.argv[1], sys.argv[2]))
