# SPDX-License-Identifier: LGPL-2.1-or-later

import tests as xdp

import dbus
import pytest


@pytest.fixture
def app_id():
    return "org.example.WrongAppId"


@pytest.fixture
def required_templates():
    return {"remotedesktop": {}}


class TestRegistry:
    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Registry", 1, domain="host")

    def create_dummy_session(self, dbus_con):
        remotedesktop_intf = xdp.get_portal_iface(dbus_con, "RemoteDesktop")
        request = xdp.Request(dbus_con, remotedesktop_intf)

        session_counter_attr_name = "session_counter"
        if hasattr(self, session_counter_attr_name):
            session_counter = getattr(self, session_counter_attr_name)
        else:
            session_counter = 0
        setattr(self, session_counter_attr_name, session_counter + 1)

        print(f"session_handle_token: {session_counter}")
        options = {
            "session_handle_token": f"session_token{session_counter}",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )
        assert response.response == 0

        return xdp.Session.from_response(dbus_con, response)

    def test_registerless(self, portals, dbus_con):
        mock_intf = xdp.get_mock_iface(dbus_con)

        expected_app_id = "org.example.WrongAppId"

        session = self.create_dummy_session(dbus_con)

        app_id = mock_intf.GetSessionAppId(session.handle)
        assert app_id == expected_app_id

    def test_register(self, portals, dbus_con):
        registry_intf = xdp.get_portal_iface(dbus_con, "Registry", domain="host")
        mock_intf = xdp.get_mock_iface(dbus_con)

        expected_app_id = "org.example.CorrectAppId"
        registry_intf.Register(expected_app_id, {})

        session = self.create_dummy_session(dbus_con)

        app_id = mock_intf.GetSessionAppId(session.handle)
        assert app_id == expected_app_id

    def test_late_register(self, portals, dbus_con):
        registry_intf = xdp.get_portal_iface(dbus_con, "Registry", domain="host")
        mock_intf = xdp.get_mock_iface(dbus_con)

        expected_app_id = "org.example.WrongAppId"
        unexpected_app_id = "org.example.CorrectAppId"

        session = self.create_dummy_session(dbus_con)

        app_id = mock_intf.GetSessionAppId(session.handle)
        assert app_id == expected_app_id

        with pytest.raises(dbus.exceptions.DBusException):
            registry_intf.Register(unexpected_app_id, {})

        new_session = self.create_dummy_session(dbus_con)

        new_app_id = mock_intf.GetSessionAppId(new_session.handle)
        assert new_app_id == expected_app_id

    def test_multiple_connections(self, portals, dbus_con):
        registry_intf = xdp.get_portal_iface(dbus_con, "Registry", domain="host")
        mock_intf = xdp.get_mock_iface(dbus_con)

        expected_app_id = "org.example.CorrectAppId"

        registry_intf.Register(expected_app_id, {})
        session = self.create_dummy_session(dbus_con)
        app_id = mock_intf.GetSessionAppId(session.handle)
        assert app_id == expected_app_id

        dbus_con2 = dbus.bus.BusConnection(dbus.bus.BusConnection.TYPE_SESSION)
        dbus_con2.set_exit_on_disconnect(False)
        mock_intf2 = xdp.get_mock_iface(dbus_con2)
        session2 = self.create_dummy_session(dbus_con2)
        app_id2 = mock_intf2.GetSessionAppId(session2.handle)
        assert app_id2 == expected_app_id
        dbus_con2.close()
