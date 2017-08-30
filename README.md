To install

    make
    make install
    make samples

Configure the file in /etc/asterisk/stasis_amqp.conf

You need to have res_amqp.so loaded.

Please restart asterisk before loading res_stasis_amqp.so for the documentation.

To load module

    CLI> module load res_stasis_amqp.so
