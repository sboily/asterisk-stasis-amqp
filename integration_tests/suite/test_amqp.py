import ari as ari_client
from ari.exceptions import ARINotFound
import logging
import os
import pytest
from hamcrest import *
from hamcrest import assert_that
from hamcrest import calling
from hamcrest import has_property
from requests.exceptions import HTTPError
from xivo_test_helpers import until
from xivo_test_helpers.bus import BusClient
from xivo_test_helpers.asset_launching_test_case import AssetLaunchingTestCase
from xivo_test_helpers.hamcrest.raises import raises
from functools import partial

import time

log_level = logging.DEBUG if os.environ.get('TEST_LOGS') == 'verbose' else logging.INFO
logging.basicConfig(level=log_level)

app_name_key = 'applicationName'

subscribe_args = {app_name_key: 'newstasisapplication'}

class AssetLauncher(AssetLaunchingTestCase):

    assets_root = os.path.join(os.path.dirname(__file__), '..', 'assets')
    asset = 'amqp'
    service = 'ari_amqp'


@pytest.fixture()
def ari():
    AssetLauncher.kill_containers()
    AssetLauncher.rm_containers()
    AssetLauncher.launch_service_with_asset()
    ari_url = 'http://localhost:{port}'.format(port=AssetLauncher.service_port(5039, 'ari_amqp'))
    ari = until.return_(ari_client.connect, ari_url, 'wazo', 'wazo', timeout=5, interval=0.1)
    AssetLauncher.docker_exec(["asterisk", "-rx", "module load res_stasis_amqp.so"], service_name='ari_amqp')
    AssetLauncher.docker_exec(["asterisk", "-rx", "module load res_ari_amqp.so"], service_name='ari_amqp')
    yield ari
    AssetLauncher.kill_containers()

def test_stasis_amqp_events(ari):
    ari.amqp.stasisSubscribe(**subscribe_args)
    assert_that(ari.applications.list(), has_item(has_entry('name', subscribe_args[app_name_key])))

    bus_client = BusClient.from_connection_fields(port=AssetLauncher.service_port(5672, 'rabbitmq'))

    assert bus_client.is_up()

    events = bus_client.accumulator("stasis.app." + subscribe_args[app_name_key].lower())

    ari.channels.originate(endpoint='local/3000@default', extension=subscribe_args[app_name_key])

    def event_received(events, app):
        assert_that(events.accumulate(), has_item(
            has_entry('data',
                has_entry('application', app)
            )
        ))
    until.assert_(event_received, events, subscribe_args[app_name_key], timeout=50)

def test_app_subscribe(ari):
    assert_that(
        calling(ari.amqp.stasisSubscribe).with_args(**subscribe_args),
            not_(raises(Exception))
    )

    assert_that(ari.applications.list(), has_item(has_entry('name', subscribe_args[app_name_key])))

def test_app_unsubscribe(ari):
    """
    Test passes, but operation does not work for now; a tiny Asterisk patch is required.
    """
    ari.amqp.stasisSubscribe(**subscribe_args)
    assert_that(
        calling(ari.amqp.stasisUnsubscribe).with_args(**subscribe_args),
            not_(raises(Exception))
    )
