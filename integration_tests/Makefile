test-setup:
	docker pull wazopbx/wait
	docker build -t asterisk_amqp ..
	docker pull rabbitmq

test:
	py.test -x suite