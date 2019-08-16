test-setup:
	docker pull debian:stretch
	docker pull wazopbx/wait
	docker build --no-cache -t asterisk_amqp ..
	docker pull rabbitmq

test:
	py.test -x suite