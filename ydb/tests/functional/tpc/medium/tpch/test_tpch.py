import ydb.tests.olap.load.lib.tpch as tpch
from ydb.tests.functional.tpc.lib.conftest import FunctionalTestBase


class TestTpchS1(tpch.TestTpch1, FunctionalTestBase):
    iterations: int = 1

    @classmethod
    def addition_init_params(cls) -> list[str]:
        if cls.float_mode:
            return ['--float-mode', cls.float_mode]
        return []

    @classmethod
    def setup_class(cls) -> None:
        cls.setup_cluster()
        cls.run_cli(['workload', 'tpch', '-p', f'olap_yatests/{cls._get_path()}', 'init', '--store=column'] + cls.addition_init_params())
        cls.run_cli(['workload', 'tpch', '-p', f'olap_yatests/{cls._get_path()}', 'import', 'generator', f'--scale={cls.scale}'])
        super().setup_class()


class TestTpchS1Decimal_22_9(TestTpchS1):
    float_mode = 'decimal_ydb'


class TestTpchS0_1(TestTpchS1):
    tables_size: dict[str, int] = {
        'lineitem': 600572,
    }
    scale: float = 0.1
