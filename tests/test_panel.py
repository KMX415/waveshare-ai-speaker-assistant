import http.client
import json
import re
import threading
import unittest
from home_voice.panel import create_panel_server


class FakeController:
    def __init__(self): self.actions = []
    def status(self): return {"key_set": False}
    def action(self, name, data):
        self.actions.append((name,data))
        return {"ok":True}


class PanelTests(unittest.TestCase):
    def setUp(self):
        self.controller = FakeController()
        self.server = create_panel_server(self.controller,0)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever,daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join()

    def request(self, method, path, body=None, headers=None):
        client = http.client.HTTPConnection('127.0.0.1',self.port,timeout=2)
        client.request(method,path,body,headers or {})
        response=client.getresponse()
        result=(response.status,response.read())
        client.close()
        return result

    def test_foreign_origin_cannot_change_credentials(self):
        status,_=self.request('POST','/api/key','{"key":"not-a-real-key"}',
                              {'Origin':'https://untrusted.example','Content-Type':'application/json'})
        self.assertEqual(status,403)
        self.assertEqual(self.controller.actions,[])

    def test_dns_rebinding_host_is_rejected(self):
        status,_=self.request('GET','/api/status',headers={'Host':f'untrusted.example:{self.port}'})
        self.assertEqual(status,403)

    def test_local_page_can_submit_with_csrf_token(self):
        status,page=self.request('GET','/')
        self.assertEqual(status,200)
        token=re.search(rb"'X-Home-Voice':'([^']+)'",page).group(1).decode()
        headers={'Origin':f'http://127.0.0.1:{self.port}','X-Home-Voice':token}
        status,_=self.request('POST','/api/tone','{}',headers)
        self.assertEqual(status,200)
        self.assertEqual(self.controller.actions,[('tone',{})])
        status,data=self.request('GET','/api/status')
        self.assertEqual(json.loads(data),{'key_set':False})
