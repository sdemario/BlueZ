#!/usr/bin/python

'''
Proximity Monitor test script
'''

import gobject

import sys
import dbus
import dbus.mainloop.glib
from optparse import OptionParser, make_option

def property_changed(name, value):

	print "PropertyChanged('%s', '%s')" % (name, value)
	mainloop.quit()

if __name__ == "__main__":
	dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)

	bus = dbus.SystemBus()

	manager = dbus.Interface(bus.get_object("org.bluez", "/"),
					"org.bluez.Manager")

	option_list = [
		make_option("-i", "--adapter", action="store",
			type="string", dest="dev_id"),
		make_option("-b", "--device", action="store",
			type="string", dest="address"),

		]
	parser = OptionParser(option_list=option_list)

	(options, args) = parser.parse_args()

	if options.dev_id:
		adapter_path = manager.FindAdapter(options.dev_id)
	else:
		adapter_path = manager.DefaultAdapter()

	adapter = dbus.Interface(bus.get_object("org.bluez", adapter_path),
							"org.bluez.Adapter")

	if (len(args) < 1):
		print "Usage: %s <command>" % (sys.argv[0])
		print ""
		print "  LinkLossAlertLevel <none|mild|high>"
		sys.exit(1)

	device_path = adapter.FindDevice(options.address)

	bus.add_signal_receiver(property_changed, bus_name="org.bluez",
				dbus_interface="org.bluez.Proximity",
				signal_name="PropertyChanged")

	proximity = dbus.Interface(bus.get_object("org.bluez",
					device_path), "org.bluez.Proximity")

	print "Proximity SetProperty('%s', '%s')" % (args[0], args[1])
	proximity.SetProperty(args[0], args[1])

	mainloop = gobject.MainLoop()
	mainloop.run()
