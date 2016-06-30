# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from marionette import MarionetteTestCase
from marionette_driver.errors import SessionNotCreatedException


class TestCapabilities(MarionetteTestCase):

    def setUp(self):
        super(TestCapabilities, self).setUp()
        self.caps = self.marionette.session_capabilities
        with self.marionette.using_context("chrome"):
            self.appinfo = self.marionette.execute_script(
                "return Services.appinfo")
            self.os_name = self.marionette.execute_script(
                "return Services.sysinfo.getProperty('name')")
            self.os_version = self.marionette.execute_script(
                "return Services.sysinfo.getProperty('version')")

    @property
    def is_b2g(self):
        return self.appinfo["name"] == "B2G"

    def test_mandates_capabilities(self):
        self.assertIn("browserName", self.caps)
        self.assertIn("browserVersion", self.caps)
        self.assertIn("platformName", self.caps)
        self.assertIn("platformVersion", self.caps)
        self.assertIn("specificationLevel", self.caps)

        self.assertEqual(self.caps["browserName"], self.appinfo["name"])
        self.assertEqual(self.caps["browserVersion"], self.appinfo["version"])
        self.assertEqual(self.caps["platformName"], self.os_name)
        self.assertEqual(self.caps["platformVersion"], self.os_version)
        self.assertEqual(self.caps["specificationLevel"], 0)

    def test_supported_features(self):
        self.assertIn("rotatable", self.caps)
        self.assertIn("acceptSslCerts", self.caps)
        self.assertIn("takesElementScreenshot", self.caps)
        self.assertIn("takesScreenshot", self.caps)

        self.assertEqual(self.caps["rotatable"], self.is_b2g)
        self.assertFalse(self.caps["acceptSslCerts"])
        self.assertTrue(self.caps["takesElementScreenshot"])
        self.assertTrue(self.caps["takesScreenshot"])

    def test_selenium2_compat(self):
        self.assertIn("platform", self.caps)
        self.assertEqual(self.caps["platform"], self.caps["platformName"].upper())

    def test_extensions(self):
        self.assertIn("XULappId", self.caps)
        self.assertIn("appBuildId", self.caps)
        self.assertIn("version", self.caps)

        self.assertEqual(self.caps["XULappId"], self.appinfo["ID"])
        self.assertEqual(self.caps["appBuildId"], self.appinfo["appBuildID"])
        self.assertEqual(self.caps["version"], self.appinfo["version"])

    def test_we_can_pass_in_capabilities_on_session_start(self):
        self.marionette.delete_session()
        capabilities = {"desiredCapabilities": {"somethingAwesome": "cake"}}
        self.marionette.start_session(capabilities)
        caps = self.marionette.session_capabilities
        self.assertIn("somethingAwesome", caps)

    def test_set_specification_level(self):
        self.marionette.delete_session()
        self.marionette.start_session({"specificationLevel": 1})
        caps = self.marionette.session_capabilities
        self.assertEqual(1, caps["specificationLevel"])

    def test_we_dont_overwrite_server_capabilities(self):
        self.marionette.delete_session()
        capabilities = {"desiredCapabilities": {"browserName": "ChocolateCake"}}
        self.marionette.start_session(capabilities)
        caps = self.marionette.session_capabilities
        self.assertEqual(caps["browserName"], self.appinfo["name"],
                         "This should have appname not ChocolateCake")

    def test_we_can_pass_in_required_capabilities_on_session_start(self):
        self.marionette.delete_session()
        capabilities = {"requiredCapabilities": {"browserName": self.appinfo["name"]}}
        self.marionette.start_session(capabilities)
        caps = self.marionette.session_capabilities
        self.assertIn("browserName", caps)

    def test_we_pass_in_required_capability_we_cant_fulfil_raises_exception(self):
        self.marionette.delete_session()
        capabilities = {"requiredCapabilities": {"browserName": "CookiesAndCream"}}
        try:
            self.marionette.start_session(capabilities)
            self.fail("Marionette Should have throw an exception")
        except SessionNotCreatedException as e:
            # We want an exception
            self.assertIn("CookiesAndCream does not equal", str(e))

        # Start a new session just to make sure we leave the browser in the
        # same state it was before it started the test
        self.marionette.start_session()

    def test_we_get_valid_uuid4_when_creating_a_session(self):
        self.assertNotIn("{", self.marionette.session_id,
                         "Session ID has {} in it: %s" % self.marionette.session_id)
