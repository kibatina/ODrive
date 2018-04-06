
import sys
import platform
import threading
import odrive.discovery
from odrive.utils import start_liveplotter
from odrive.enums import * # pylint: disable=W0614

def print_banner():
    print('Please connect your ODrive.')
    print('Type help() for help.')

def print_help(args):
    print('')
    if len(discovered_devices) == 0:
        print('Connect your ODrive to {} and power it up.'.format(args.path))
        print('After that, the following message should appear:')
        print('  "Connected to ODrive [serial number] as odrv0"')
        print('')
        print('Once the ODrive is connected, type "odrv0." and press <tab>')
    else:
        print('Type "odrv0." and press <tab>')
    print('This will present you with all the properties that you can reference')
    print('')
    print('For example: "odrv0.motor0.encoder.pll_pos"')
    print('will print the current encoder position on motor 0')
    print('and "odrv0.motor0.pos_setpoint = 10000"')
    print('will send motor0 to 10000')
    print('')


interactive_variables = {}

discovered_devices = []

def did_discover_device(odrive, logger, app_shutdown_token):
    """
    Handles the discovery of new devices by displaying a
    message and making the device available to the interactive
    console
    """
    serial_number = odrive.serial_number if hasattr(odrive, 'serial_number') else "[unknown serial number]"
    if serial_number in discovered_devices:
        verb = "Reconnected"
        index = discovered_devices.index(serial_number)
    else:
        verb = "Connected"
        discovered_devices.append(serial_number)
        index = len(discovered_devices) - 1
    interactive_name = "odrv" + str(index)

    # Publish new ODrive to interactive console
    interactive_variables[interactive_name] = odrive
    globals()[interactive_name] = odrive # Add to globals so tab complete works
    logger.info("{} to ODrive {:012X} as {}".format(verb, serial_number, interactive_name))

    # Subscribe to disappearance of the device
    odrive.__channel__._channel_broken.subscribe(lambda: did_lose_device(interactive_name, logger, app_shutdown_token))

def did_lose_device(interactive_name, logger, app_shutdown_token):
    """
    Handles the disappearance of a device by displaying
    a message.
    """
    if not app_shutdown_token.is_set():
        logger.warn("Oh no {} disappeared".format(interactive_name))

def launch_shell(args, logger, printer, app_shutdown_token):
    """
    Launches an interactive python or IPython command line
    interface.
    As ODrives are connected they are made available as
    "odrv0", "odrv1", ...
    """

    # Connect to device
    logger.debug("Waiting for device...")
    odrive.discovery.find_all(args.path, args.serial_number,
                    lambda dev: did_discover_device(dev, logger, app_shutdown_token),
                    app_shutdown_token,
                    printer=printer)

    # Check if IPython is installed
    if args.no_ipython:
        use_ipython = False
    else:
        try:
            import IPython
            use_ipython = True
        except:
            print("Warning: you don't have IPython installed.")
            print("If you want to have an improved interactive console with pretty colors,")
            print("you should install IPython\n")
            use_ipython = False

    interactive_variables["help"] = lambda: print_help(args)

    # If IPython is installed, embed IPython shell, otherwise embed regular shell
    if use_ipython:
        help = lambda: print_help(args) # Override help function # pylint: disable=W0612
        console = IPython.terminal.embed.InteractiveShellEmbed(local_ns=interactive_variables, banner1='')
        console.runcode = console.run_code # hack to make IPython look like the regular console
        interact = console
    else:
        # Enable tab complete if possible
        try:
            import readline # Works only on Unix
            readline.parse_and_bind("tab: complete")
        except:
            sudo_prefix = "" if platform.system() == "Windows" else "sudo "
            print("Warning: could not enable tab-complete. User experience will suffer.\n"
                "Run `{}pip install readline` and then restart this script to fix this."
                .format(sudo_prefix))

        import code
        console = code.InteractiveConsole(locals=interactive_variables)
        interact = lambda: console.interact(banner='')

    # install hook to hide ChannelBrokenException
    console.runcode('import sys')
    console.runcode('superexcepthook = sys.excepthook')
    console.runcode('def newexcepthook(ex_class,ex,trace):\n'
                    '  if ex_class.__module__ + "." + ex_class.__name__ != "odrive.protocol.ChannelBrokenException":\n'
                    '    superexcepthook(ex_class,ex,trace)')
    console.runcode('sys.excepthook=newexcepthook')


    # Launch shell
    print_banner()
    logger._skip_bottom_line = True
    interact()
    app_shutdown_token.set()