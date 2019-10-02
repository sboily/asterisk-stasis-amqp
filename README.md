To build the module you will need the following dependencies

* asterisk-dev
* wazo-res-amqp-dev
* librabbitmq-dev

To install

    make
    make install
    make samples

Configure the file in /etc/asterisk/stasis_amqp.conf

You need to have res_amqp.so loaded.

Please restart asterisk before loading res_stasis_amqp.so for the documentation.

To load module

    CLI> module load res_stasis_amqp.so

Events is push for:

* channel in routing key `stasis.channel.<channel uniqueid>`
* ari apps in routing key `stasis.app.<app name>`
* ami in routing key `stasis.ami.<event name>`

##### Missing

* Unsubscribe from Stasis functionality not yet implemented.